/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kode_button.c
 * @brief The standard layer's buttons: the kode_button_*() calls.
 */
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "unity.h"

#include "ao_spy.h"
#include "fakes.h"
#include "kaos.h"
#include "kdrv_buttons.h"
#include "kode.h"
#include "kode_button.h"
#include "kode_sys_priv.h"
#include "qf_support.h"

/** @brief The id of every button, in kdrv_button_t's order, for loops. */
#define TEST_BUTTONS ((uint8_t)KDRV_BTN_COUNT)

/** @brief A count a little below the 32-bit wrap. */
#define TEST_NEAR_WRAP 0xFFFFFFFEU

/** @brief The count the wrap case reads next, after the wrap. */
#define TEST_AFTER_WRAP 3U

/** @brief The presses between TEST_NEAR_WRAP and TEST_AFTER_WRAP. */
#define TEST_WRAP_DELTA 5

/** @brief The number of presses a counting case makes. */
#define TEST_PRESSES 3U

/** @brief What the wrap case's reader answers. */
static uint32_t s_reader_value;

/** @brief Make kode_init() a no-op, as it is once the system is up. */
static void system_is_up(void)
{
    atomic_store(&kode_sys_init_once.state, KHAL_ONCE_DONE);
}

/** @brief Bring the event system up so a press can be published, and let go of every button. */
static void pad_up(void)
{
    qf_support_init();
    (void)ao_spy();
    ao_spy_drain();
    system_is_up();
    for ( uint8_t id = 0U; TEST_BUTTONS > id; id++ )
    {
        (void)buttons_inject(id, false);
    }
    ao_spy_drain();
}

/** @brief Press and release button @p id once. */
static void tap(kdrv_button_t id)
{
    TEST_ASSERT_TRUE(buttons_inject((uint8_t)id, true));
    TEST_ASSERT_TRUE(buttons_inject((uint8_t)id, false));
    ao_spy_drain();
}

/** @brief Forget every press up to now, on every button. */
static void spend_news(void)
{
    (void)kode_button_get_presses("up");
    (void)kode_button_get_presses("down");
    (void)kode_button_get_presses("left");
    (void)kode_button_get_presses("right");
    (void)kode_button_get_presses("top");
    (void)kode_button_get_presses("bottom");
}

/** @brief The reader the wrap case hands to kode_button_take(). */
static uint32_t reader_fake(uint8_t id)
{
    (void)id;
    return s_reader_value;
}

TEST_CASE("is_pressed reads the mask", "[kode_button]")
{
    pad_up();
    TEST_ASSERT_FALSE(kode_button_is_pressed("top"));

    TEST_ASSERT_TRUE(buttons_inject((uint8_t)KDRV_BTN_TOP, true));
    ao_spy_drain();
    TEST_ASSERT_TRUE(kode_button_is_pressed("top"));
    TEST_ASSERT_FALSE(kode_button_is_pressed("up"));
    TEST_ASSERT_FALSE(kode_button_is_pressed("bottom"));
    TEST_ASSERT_EQUAL_STRING("", kode_error());

    TEST_ASSERT_TRUE(buttons_inject((uint8_t)KDRV_BTN_TOP, false));
    ao_spy_drain();
    TEST_ASSERT_FALSE(kode_button_is_pressed("top"));
}

TEST_CASE("names are case-insensitive", "[kode_button]")
{
    pad_up();
    TEST_ASSERT_TRUE(buttons_inject((uint8_t)KDRV_BTN_LEFT, true));
    ao_spy_drain();

    TEST_ASSERT_TRUE(kode_button_is_pressed("left"));
    TEST_ASSERT_TRUE(kode_button_is_pressed("LEFT"));
    TEST_ASSERT_TRUE(kode_button_is_pressed("Left"));
    TEST_ASSERT_EQUAL_STRING("", kode_error());

    TEST_ASSERT_TRUE(buttons_inject((uint8_t)KDRV_BTN_LEFT, false));
    ao_spy_drain();
}

TEST_CASE("a leading space fails and the error names the name as typed", "[kode_button]")
{
    pad_up();

    TEST_ASSERT_FALSE(kode_button_is_pressed(" Top"));
    TEST_ASSERT_EQUAL_STRING("unknown button ' Top'", kode_error());
    TEST_ASSERT_FALSE(kode_button_was_pressed(" Top"));
    TEST_ASSERT_EQUAL_STRING("unknown button ' Top'", kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, kode_button_get_presses(" Top"));
    TEST_ASSERT_EQUAL_STRING("unknown button ' Top'", kode_error());
}

TEST_CASE("bot suggests bottom, any other unknown name does not", "[kode_button]")
{
    pad_up();

    TEST_ASSERT_FALSE(kode_button_is_pressed("bot"));
    TEST_ASSERT_EQUAL_STRING("unknown button 'bot' (did you mean 'bottom'?)", kode_error());

    TEST_ASSERT_FALSE(kode_button_is_pressed("middle"));
    TEST_ASSERT_EQUAL_STRING("unknown button 'middle'", kode_error());

    TEST_ASSERT_FALSE(kode_button_is_pressed(""));
    TEST_ASSERT_EQUAL_STRING("unknown button ''", kode_error());

    TEST_ASSERT_FALSE(kode_button_is_pressed("bottoms"));
    TEST_ASSERT_EQUAL_STRING("unknown button 'bottoms'", kode_error());
}

TEST_CASE("was_pressed latches until it is read", "[kode_button]")
{
    pad_up();
    spend_news();
    TEST_ASSERT_FALSE(kode_button_was_pressed("down"));

    tap(KDRV_BTN_DOWN);
    TEST_ASSERT_FALSE_MESSAGE(kode_button_is_pressed("down"), "the tap is over");
    TEST_ASSERT_TRUE_MESSAGE(kode_button_was_pressed("down"), "but it is not forgotten");
    TEST_ASSERT_FALSE_MESSAGE(kode_button_was_pressed("down"), "and it is told once");
    TEST_ASSERT_FALSE_MESSAGE(kode_button_was_pressed("up"), "to its own button only");
    TEST_ASSERT_EQUAL_STRING("", kode_error());
}

TEST_CASE("get_presses counts, then resets", "[kode_button]")
{
    pad_up();
    spend_news();

    for ( uint32_t i = 0U; TEST_PRESSES > i; i++ )
    {
        tap(KDRV_BTN_RIGHT);
    }
    TEST_ASSERT_EQUAL_UINT32(TEST_PRESSES, kode_button_get_presses("right"));
    TEST_ASSERT_EQUAL_UINT32(0U, kode_button_get_presses("right"));
    TEST_ASSERT_EQUAL_STRING("", kode_error());

    tap(KDRV_BTN_RIGHT);
    TEST_ASSERT_EQUAL_UINT32(1U, kode_button_get_presses("RIGHT"));
}

TEST_CASE("was_pressed and get_presses share one count", "[kode_button]")
{
    pad_up();
    spend_news();

    tap(KDRV_BTN_UP);
    tap(KDRV_BTN_UP);
    TEST_ASSERT_TRUE(kode_button_was_pressed("up"));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, kode_button_get_presses("up"),
                                  "asking one consumes the other's news");

    tap(KDRV_BTN_UP);
    TEST_ASSERT_EQUAL_UINT32(1U, kode_button_get_presses("up"));
    TEST_ASSERT_FALSE(kode_button_was_pressed("up"));
}

TEST_CASE("get_presses counts across the 32-bit wrap", "[kode_button]")
{
    pad_up();

    s_reader_value = TEST_NEAR_WRAP;
    (void)kode_button_take((uint8_t)KDRV_BTN_UP, &reader_fake);

    s_reader_value = TEST_AFTER_WRAP;
    TEST_ASSERT_EQUAL_UINT32((uint32_t)TEST_WRAP_DELTA,
                             kode_button_take((uint8_t)KDRV_BTN_UP, &reader_fake));
    TEST_ASSERT_EQUAL_UINT32(0U, kode_button_take((uint8_t)KDRV_BTN_UP, &reader_fake));

    spend_news();
}

TEST_CASE("read_all matches the mask", "[kode_button]")
{
    pad_up();

    kode_buttons_t none = kode_button_read_all();
    TEST_ASSERT_FALSE(none.up || none.down || none.left || none.right || none.top || none.bottom);

    TEST_ASSERT_TRUE(buttons_inject((uint8_t)KDRV_BTN_UP, true));
    TEST_ASSERT_TRUE(buttons_inject((uint8_t)KDRV_BTN_RIGHT, true));
    TEST_ASSERT_TRUE(buttons_inject((uint8_t)KDRV_BTN_BOT, true));
    ao_spy_drain();

    kode_buttons_t some = kode_button_read_all();
    TEST_ASSERT_TRUE(some.up);
    TEST_ASSERT_FALSE(some.down);
    TEST_ASSERT_FALSE(some.left);
    TEST_ASSERT_TRUE(some.right);
    TEST_ASSERT_FALSE(some.top);
    TEST_ASSERT_TRUE(some.bottom);
    TEST_ASSERT_EQUAL_STRING("", kode_error());

    for ( uint8_t id = 0U; TEST_BUTTONS > id; id++ )
    {
        (void)buttons_inject(id, false);
    }
    ao_spy_drain();
}

TEST_CASE("a NULL name fails", "[kode_button]")
{
    pad_up();

    TEST_ASSERT_FALSE(kode_button_is_pressed(NULL));
    TEST_ASSERT_EQUAL_STRING("button name is NULL", kode_error());
    TEST_ASSERT_FALSE(kode_button_was_pressed(NULL));
    TEST_ASSERT_EQUAL_STRING("button name is NULL", kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, kode_button_get_presses(NULL));
    TEST_ASSERT_EQUAL_STRING("button name is NULL", kode_error());
}

TEST_CASE("every button has its name, and an id out of range has none", "[kode_button]")
{
    TEST_ASSERT_EQUAL_STRING("up", kode_button_name((uint8_t)KDRV_BTN_UP));
    TEST_ASSERT_EQUAL_STRING("down", kode_button_name((uint8_t)KDRV_BTN_DOWN));
    TEST_ASSERT_EQUAL_STRING("left", kode_button_name((uint8_t)KDRV_BTN_LEFT));
    TEST_ASSERT_EQUAL_STRING("right", kode_button_name((uint8_t)KDRV_BTN_RIGHT));
    TEST_ASSERT_EQUAL_STRING("top", kode_button_name((uint8_t)KDRV_BTN_TOP));
    TEST_ASSERT_EQUAL_STRING("bottom", kode_button_name((uint8_t)KDRV_BTN_BOT));
    TEST_ASSERT_NULL(kode_button_name(TEST_BUTTONS));
}
