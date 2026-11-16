/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_lowpower_part.c
 * @brief The rules of an app's rest: what a request may have, and the order of the parts.
 */
#include <stdint.h>
#include <string.h>

#include "unity.h"

#include "kqp.h"
#include "lowpower_part.h"

/** @brief A request judged with nothing at rest, a glass, no cable and no wake source. */
static const lowpower_part_ask_t PLAIN = { .at_rest = 0U, .glass = true };

/** @brief Assert that @p parts stands and becomes @p rested. */
static void stands(uint8_t parts, const lowpower_part_ask_t *ask, uint8_t rested)
{
    uint8_t mask = parts;

    TEST_ASSERT_NULL(lowpower_part_check(&mask, ask));
    TEST_ASSERT_EQUAL_HEX8(rested, mask);
}

/** @brief Assert that @p parts is refused with @p why and left as it came. */
static void refused(uint8_t parts, const lowpower_part_ask_t *ask, const char *why)
{
    uint8_t mask = parts;
    const char * const got = lowpower_part_check(&mask, ask);

    TEST_ASSERT_NOT_NULL(got);
    TEST_ASSERT_EQUAL_STRING(why, got);
    TEST_ASSERT_EQUAL_HEX8(parts, mask);
}

TEST_CASE("a part alone stands as it was asked", "[lowpower_part]")
{
    stands(KODE_LP_PART_SCREEN, &PLAIN, KODE_LP_PART_SCREEN);
    stands(KODE_LP_PART_CARD | KODE_LP_PART_SOUND, &PLAIN,
           KODE_LP_PART_CARD | KODE_LP_PART_SOUND);
}

TEST_CASE("no part, and a bit that is no part, are refused", "[lowpower_part]")
{
    refused(0U, &PLAIN, "request: no such part");
    refused(0x80U, &PLAIN, "request: no such part");
}

TEST_CASE("the panel brings the screen and the touch with it", "[lowpower_part]")
{
    stands(KODE_LP_PART_PANEL, &PLAIN,
           KODE_LP_PART_PANEL | KODE_LP_PART_SCREEN | KODE_LP_PART_TOUCH);
}

/* The sound comes back by itself at its next use, so an earlier request's
 * word for it is worth nothing: its owner is asked in the radio's own. */
TEST_CASE("the radio is refused without the sound in the same request", "[lowpower_part]")
{
    lowpower_part_ask_t held = PLAIN;

    refused(KODE_LP_PART_RADIO, &PLAIN, "radio: needs sound at rest");
    stands(KODE_LP_PART_RADIO | KODE_LP_PART_SOUND, &PLAIN,
           KODE_LP_PART_RADIO | KODE_LP_PART_SOUND);
    held.at_rest = KODE_LP_PART_SOUND | KODE_LP_PART_TOUCH;
    refused(KODE_LP_PART_RADIO, &held, "radio: needs sound at rest");
    /* With the radio held already, what is added needs no sound. */
    held.at_rest = KODE_LP_PART_RADIO;
    stands(KODE_LP_PART_TOUCH, &held, KODE_LP_PART_TOUCH);
}

TEST_CASE("the CPU is refused while the glass is up, and not without a glass", "[lowpower_part]")
{
    lowpower_part_ask_t ask = PLAIN;

    refused(KODE_LP_PART_CPU, &ask, "cpu: needs the screen at rest");
    stands(KODE_LP_PART_CPU | KODE_LP_PART_SCREEN, &ask,
           KODE_LP_PART_CPU | KODE_LP_PART_SCREEN);
    ask.at_rest = KODE_LP_PART_SCREEN;
    stands(KODE_LP_PART_CPU, &ask, KODE_LP_PART_CPU);
    ask.at_rest = 0U;
    ask.glass   = false;
    stands(KODE_LP_PART_CPU, &ask, KODE_LP_PART_CPU);
}

TEST_CASE("the CPU is refused on a cable, the other parts are not", "[lowpower_part]")
{
    lowpower_part_ask_t ask = PLAIN;

    ask.cable = true;
    refused(KODE_LP_PART_CPU | KODE_LP_PART_SCREEN, &ask, "cable: external power");
    stands(KODE_LP_PART_SCREEN | KODE_LP_PART_CARD, &ask,
           KODE_LP_PART_SCREEN | KODE_LP_PART_CARD);
}

TEST_CASE("a touch cannot wake with the touch at rest", "[lowpower_part]")
{
    lowpower_part_ask_t ask = PLAIN;

    ask.wake_touch = true;
    stands(KODE_LP_PART_CPU | KODE_LP_PART_SCREEN, &ask,
           KODE_LP_PART_CPU | KODE_LP_PART_SCREEN);
    refused(KODE_LP_PART_CPU | KODE_LP_PART_SCREEN | KODE_LP_PART_TOUCH, &ask,
            "touch: at rest cannot wake");
    refused(KODE_LP_PART_CPU | KODE_LP_PART_PANEL, &ask, "touch: at rest cannot wake");
    /* Without the CPU part the wake sources are not read. */
    stands(KODE_LP_PART_TOUCH, &ask, KODE_LP_PART_TOUCH);
}

TEST_CASE("a line the header does not have is refused", "[lowpower_part]")
{
    lowpower_part_ask_t ask = PLAIN;

    ask.wake_pin = true;
    refused(KODE_LP_PART_CPU | KODE_LP_PART_SCREEN, &ask, "pin: no such position");
    ask.pin_known = true;
    ask.pin_free  = true;
    stands(KODE_LP_PART_CPU | KODE_LP_PART_SCREEN, &ask,
           KODE_LP_PART_CPU | KODE_LP_PART_SCREEN);
}

/* An output, a PWM or a loan to a driver has an owner using it. */
TEST_CASE("a line of the header that is in use is refused", "[lowpower_part]")
{
    lowpower_part_ask_t ask = PLAIN;

    ask.wake_pin  = true;
    ask.pin_known = true;
    refused(KODE_LP_PART_CPU | KODE_LP_PART_SCREEN, &ask, "pin: in use");
}

TEST_CASE("the parts go down in their order, the sound before the radio and the CPU last",
          "[lowpower_part]")
{
    static const uint8_t order[] = {
        KODE_LP_PART_SOUND, KODE_LP_PART_CARD,  KODE_LP_PART_TOUCH, KODE_LP_PART_SCREEN,
        KODE_LP_PART_PANEL, KODE_LP_PART_RADIO, KODE_LP_PART_CPU,
    };
    uint8_t at_rest = 0U;

    for ( uint8_t i = 0U; Q_DIM(order) > i; i++ )
    {
        uint8_t const next = lowpower_part_next_down(KODE_LP_PART_ALL, at_rest);

        TEST_ASSERT_EQUAL_HEX8(order[i], next);
        at_rest |= next;
    }
    TEST_ASSERT_EQUAL_HEX8(0U, lowpower_part_next_down(KODE_LP_PART_ALL, at_rest));
    /* A part at rest already is not asked again. */
    TEST_ASSERT_EQUAL_HEX8(KODE_LP_PART_CPU,
                           lowpower_part_next_down(KODE_LP_PART_SCREEN | KODE_LP_PART_CPU,
                                                   KODE_LP_PART_SCREEN));
}

TEST_CASE("the parts held come back the CPU first", "[lowpower_part]")
{
    static const uint8_t order[] = {
        KODE_LP_PART_CPU, KODE_LP_PART_RADIO, KODE_LP_PART_PANEL, KODE_LP_PART_SCREEN,
        KODE_LP_PART_TOUCH,
    };
    uint8_t at_rest = KODE_LP_PART_CPU | KODE_LP_PART_RADIO | KODE_LP_PART_PANEL
                      | KODE_LP_PART_SCREEN | KODE_LP_PART_TOUCH;

    for ( uint8_t i = 0U; Q_DIM(order) > i; i++ )
    {
        uint8_t const next = lowpower_part_next_up(at_rest);

        TEST_ASSERT_EQUAL_HEX8(order[i], next);
        at_rest &= (uint8_t)~next;
    }
    TEST_ASSERT_EQUAL_HEX8(0U, lowpower_part_next_up(at_rest));
}

/* They come back by themselves at their next use: nobody can say they rest. */
TEST_CASE("the sound and the card do not stay at rest; every other part does", "[lowpower_part]")
{
    TEST_ASSERT_FALSE(lowpower_part_stays(KODE_LP_PART_SOUND));
    TEST_ASSERT_FALSE(lowpower_part_stays(KODE_LP_PART_CARD));
    TEST_ASSERT_TRUE(lowpower_part_stays(KODE_LP_PART_TOUCH));
    TEST_ASSERT_TRUE(lowpower_part_stays(KODE_LP_PART_SCREEN));
    TEST_ASSERT_TRUE(lowpower_part_stays(KODE_LP_PART_PANEL));
    TEST_ASSERT_TRUE(lowpower_part_stays(KODE_LP_PART_RADIO));
    TEST_ASSERT_TRUE(lowpower_part_stays(KODE_LP_PART_CPU));
}

TEST_CASE("every part has a name and an owner; a mask of two has neither", "[lowpower_part]")
{
    TEST_ASSERT_EQUAL_STRING("sound", lowpower_part_name(KODE_LP_PART_SOUND));
    TEST_ASSERT_EQUAL_STRING("audio", lowpower_part_owner(KODE_LP_PART_SOUND));
    TEST_ASSERT_EQUAL_STRING("storage", lowpower_part_owner(KODE_LP_PART_CARD));
    TEST_ASSERT_EQUAL_STRING("display", lowpower_part_owner(KODE_LP_PART_PANEL));
    TEST_ASSERT_EQUAL_STRING("ioexp", lowpower_part_owner(KODE_LP_PART_CPU));
    TEST_ASSERT_EQUAL_STRING("part", lowpower_part_name(KODE_LP_PART_SOUND | KODE_LP_PART_CARD));
    TEST_ASSERT_EQUAL_STRING("", lowpower_part_owner(0U));
}
