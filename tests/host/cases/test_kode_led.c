/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kode_led.c
 * @brief The standard layer's colours and LED: kode_rgb() and the kode_led_*() calls.
 */
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "unity.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "fakes.h"
#include "kode.h"
#include "kode_sys.h"
#include "kode_sys_priv.h"
#include "kaos.h"
#include "kqp.h"
#include "qf_support.h"

/** @brief Most pool blocks the exhaust helper will hold. */
#define HOLD_MAX 64U

/** @brief How long the LED calls keep trying, in ms (the layer's bound). */
#define TEST_LED_BOUND_MS 100U

/** @brief The blue of the brief's example, 0xRRGGBB. */
#define TEST_KODY_BLUE 0x028AC2U

/** @brief Red, green and blue of the pack case: three different bytes. */
#define TEST_R 0x12U
#define TEST_G 0x34U
#define TEST_B 0x56U

/** @brief The pack case's colour, 0xRRGGBB. */
#define TEST_PACKED 0x123456U

/** @brief The pool blocks a case holds so the LED finds none. */
static LedRgbEvt *s_held[HOLD_MAX];

/** @brief How many of s_held are in hand. */
static unsigned s_held_n;

/** @brief Take every pool block there is. */
static void pool_exhaust(void)
{
    LedRgbEvt *e = Q_NEW_X(LedRgbEvt, 0U, LED_RGB_SIG);

    s_held_n = 0U;
    while ( (NULL != e) && (HOLD_MAX > s_held_n) )
    {
        s_held[s_held_n] = e;
        ++s_held_n;
        e = Q_NEW_X(LedRgbEvt, 0U, LED_RGB_SIG);
    }
}

/** @brief Give one block back: the pool has room for one event. */
static void pool_free_one(void)
{
    if ( 0U < s_held_n )
    {
        --s_held_n;
        QF_gc(&s_held[s_held_n]->super);
    }
}

/** @brief Give every block still in hand back. */
static void pool_free_all(void)
{
    while ( 0U < s_held_n )
    {
        pool_free_one();
    }
}

/** @brief Most events a case takes from AO_LedMgr's queue before it drains. */
#define TAKEN_MAX 8U

/** @brief The events taken, held until led_drain() recycles them. */
static const QEvt *s_taken[TAKEN_MAX];

/** @brief How many of s_taken are held. */
static uint32_t s_taken_n;

/** @brief How many events wait in AO_LedMgr's queue. */
static uint32_t led_count(void)
{
    return (uint32_t)uxQueueMessagesWaiting(AO_LedMgr->eQueue);
}

/** @brief Take the next event from AO_LedMgr's queue, or NULL when none waits. */
static const QEvt *led_take(void)
{
    const QEvt *e = NULL;

    if ( (TAKEN_MAX > s_taken_n) && (xQueueReceive(AO_LedMgr->eQueue, &e, 0U) == pdTRUE) )
    {
        s_taken[s_taken_n] = e;
        ++s_taken_n;
    }
    else
    {
        e = NULL;
    }
    return e;
}

/** @brief Recycle what was taken and empty AO_LedMgr's queue. */
static void led_drain(void)
{
    const QEvt *e = NULL;

    while ( 0U < s_taken_n )
    {
        --s_taken_n;
        QF_gc(s_taken[s_taken_n]);
    }
    while ( xQueueReceive(AO_LedMgr->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }
}

/** @brief A system that is up, a clean clock and AO_LedMgr's queue empty. */
static void led_fresh(void)
{
    qf_support_init();
    qf_support_margin_queue(AO_LedMgr);
    led_drain();
    atomic_store(&kode_sys_init_once.state, KHAL_ONCE_DONE);
    fake_time_reset();
}

/** @brief Check the next event is an RGB request for (r, g, b). */
static void expect_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    const LedRgbEvt *e = (const LedRgbEvt *)(const void *)led_take();

    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_INT(LED_RGB_SIG, e->super.sig);
    TEST_ASSERT_EQUAL_UINT8(r, e->r);
    TEST_ASSERT_EQUAL_UINT8(g, e->g);
    TEST_ASSERT_EQUAL_UINT8(b, e->b);
}

TEST_CASE("test_set_color_blue_posts_028ac2", "[kode_led]")
{
    led_fresh();
    TEST_ASSERT_EQUAL_HEX32(TEST_KODY_BLUE, KODE_BLUE);

    TEST_ASSERT_TRUE(kode_led_set_color(KODE_BLUE));
    TEST_ASSERT_EQUAL_UINT(1U, led_count());
    expect_rgb(0x02U, 0x8AU, 0xC2U);
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    led_drain();
}

TEST_CASE("test_rgb_packs_bytes", "[kode_led]")
{
    led_fresh();

    TEST_ASSERT_EQUAL_HEX32(TEST_PACKED, kode_rgb(TEST_R, TEST_G, TEST_B));
    TEST_ASSERT_EQUAL_HEX32(0x000000U, kode_rgb(0U, 0U, 0U));
    TEST_ASSERT_EQUAL_HEX32(0xFFFFFFU, kode_rgb(0xFFU, 0xFFU, 0xFFU));

    TEST_ASSERT_TRUE(kode_led_set_rgb(TEST_R, TEST_G, TEST_B));
    expect_rgb(TEST_R, TEST_G, TEST_B);

    TEST_ASSERT_TRUE(kode_led_set_color(KODE_WHITE));
    expect_rgb(0xFFU, 0xFFU, 0xFFU);
    led_drain();
}

TEST_CASE("test_off_posts_off", "[kode_led]")
{
    const LedEvt *e;

    led_fresh();

    TEST_ASSERT_TRUE(kode_led_off());
    TEST_ASSERT_EQUAL_UINT(1U, led_count());
    e = (const LedEvt *)(const void *)led_take();
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_INT(LED_SET_SIG, e->super.sig);
    TEST_ASSERT_FALSE(e->on);
    led_drain();
}

TEST_CASE("toggle posts the toggle and sets no error", "[kode_led]")
{
    const LedEvt *e;

    led_fresh();

    TEST_ASSERT_TRUE(kode_led_toggle());
    TEST_ASSERT_EQUAL_UINT(1U, led_count());
    e = (const LedEvt *)(const void *)led_take();
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_INT(LED_TOGGLE_SIG, e->super.sig);
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, kode_sys_time_ms(), "a taken request is not waited for");
    led_drain();
}

TEST_CASE("toggle waits for a free event and goes through", "[kode_led]")
{
    led_fresh();
    pool_exhaust();
    fake_time_delay_hook(&pool_free_one);

    TEST_ASSERT_TRUE(kode_led_toggle());
    TEST_ASSERT_EQUAL_UINT(1U, led_count());
    TEST_ASSERT_EQUAL_STRING("", kode_error());

    led_drain();
    pool_free_all();
}

TEST_CASE("toggle that never finds a free event fails after its bound", "[kode_led]")
{
    led_fresh();
    pool_exhaust();

    TEST_ASSERT_FALSE(kode_led_toggle());
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(TEST_LED_BOUND_MS, kode_sys_time_ms());
    TEST_ASSERT_EQUAL_UINT(0U, led_count());
    TEST_ASSERT_EQUAL_STRING("the LED did not take the request", kode_error());
    pool_free_all();
}

TEST_CASE("a name is the palette's colour", "[kode_led]")
{
    TEST_ASSERT_EQUAL_HEX32(0x000000U, KODE_BLACK);
    TEST_ASSERT_EQUAL_HEX32(0xFFFFFFU, KODE_WHITE);
    TEST_ASSERT_EQUAL_HEX32(0xFFAC00U, KODE_YELLOW);
    TEST_ASSERT_EQUAL_HEX32(0xC2BAB3U, KODE_GREY);
    TEST_ASSERT_EQUAL_HEX32(0x212121U, KODE_DARK_GREY);
    TEST_ASSERT_EQUAL_HEX32(0x31C45AU, KODE_GREEN);
    TEST_ASSERT_EQUAL_HEX32(0x67EA94U, KODE_LIGHT_GREEN);
    TEST_ASSERT_EQUAL_HEX32(0xC9303DU, KODE_DEEP_RED);
}

TEST_CASE("test_pure_red_is_ff0000", "[kode_led]")
{
    TEST_ASSERT_EQUAL_HEX32(0xFF0000U, KODE_RED);
    TEST_ASSERT_EQUAL_HEX32(0xFF00FFU, KODE_MAGENTA);
    TEST_ASSERT_EQUAL_HEX32(0x00FFFFU, KODE_CYAN);
    TEST_ASSERT_EQUAL_HEX32(0x00FF00U, KODE_PURE_GREEN);
    TEST_ASSERT_EQUAL_HEX32(0x0000FFU, KODE_PURE_BLUE);
    TEST_ASSERT_EQUAL_HEX32(0xFFFF00U, KODE_PURE_YELLOW);
}

TEST_CASE("an LED request waits for a free event and goes through", "[kode_led]")
{
    led_fresh();
    pool_exhaust();
    fake_time_delay_hook(&pool_free_one);

    TEST_ASSERT_TRUE(kode_led_set_rgb(TEST_R, TEST_G, TEST_B));
    TEST_ASSERT_EQUAL_UINT(1U, led_count());
    expect_rgb(TEST_R, TEST_G, TEST_B);
    TEST_ASSERT_EQUAL_STRING("", kode_error());

    led_drain();
    pool_free_all();
}

TEST_CASE("an LED request that never finds a free event fails after its bound",
          "[kode_led]")
{
    led_fresh();
    pool_exhaust();
    const uint32_t t0 = kode_sys_time_ms();

    TEST_ASSERT_FALSE(kode_led_set_color(KODE_DEEP_RED));
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(TEST_LED_BOUND_MS, kode_sys_time_ms() - t0);
    TEST_ASSERT_EQUAL_UINT(0U, led_count());
    TEST_ASSERT_EQUAL_STRING("the LED did not take the request", kode_error());

    TEST_ASSERT_FALSE(kode_led_off());
    TEST_ASSERT_EQUAL_STRING("the LED did not take the request", kode_error());

    pool_free_all();
    TEST_ASSERT_TRUE(kode_led_off());
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    led_drain();
}
