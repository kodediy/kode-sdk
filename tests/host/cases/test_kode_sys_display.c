/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kode_sys_display.c
 * @brief kode_sys_screen_brightness(): the command to AO_Display, its range and its margin.
 */
#include <stdbool.h>
#include <stdint.h>

#include "unity.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "kaos.h"
#include "kode_sys.h"
#include "kqp.h"
#include "qf_support.h"

/** @brief A level inside the lit range. */
#define TEST_LEVEL 40U

/** @brief Slots left in AO_Display's queue after the filler, for the full-queue case. */
#define TEST_ROOM_LEFT KQP_DISPLAY_CMD_MARGIN

/** @brief A static event no owner sees: it only takes a slot. */
static const QEvt s_filler = QEVT_INITIALIZER(Q_USER_SIG);

/** @brief Events waiting in AO_Display's queue. */
static uint32_t display_count(void)
{
    return (uint32_t)uxQueueMessagesWaiting(AO_Display->eQueue);
}

/** @brief Recycle everything waiting in AO_Display's queue. */
static void display_drain(void)
{
    const QEvt *e = NULL;

    while ( xQueueReceive(AO_Display->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }
}

/** @brief A clean mailbox. */
static void display_fresh(void)
{
    qf_support_init();
    qf_support_margin_queue(AO_Display);
    display_drain();
}

/**
 * @brief Check the next event is a brightness of @p pct that is not stored.
 * @param pct The level.
 */
static void expect_brightness(uint8_t pct)
{
    const QEvt *e = NULL;

    TEST_ASSERT_EQUAL_INT(pdTRUE, xQueueReceive(AO_Display->eQueue, &e, 0U));
    TEST_ASSERT_EQUAL_INT(DISPLAY_BRIGHTNESS_SIG, e->sig);
    TEST_ASSERT_EQUAL_UINT8(pct, ((const BrightnessEvt *)(const void *)e)->percent);
    TEST_ASSERT_FALSE(((const BrightnessEvt *)(const void *)e)->persist);
    QF_gc(e);
}

TEST_CASE("a level in range is posted, never stored", "[kode_sys_display]")
{
    display_fresh();

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_screen_brightness(TEST_LEVEL));
    TEST_ASSERT_EQUAL_UINT32(1U, display_count());
    expect_brightness(TEST_LEVEL);

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_screen_brightness(KODE_SYS_SCREEN_BRIGHT_MIN));
    expect_brightness(KODE_SYS_SCREEN_BRIGHT_MIN);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_screen_brightness(KODE_SYS_SCREEN_BRIGHT_MAX));
    expect_brightness(KODE_SYS_SCREEN_BRIGHT_MAX);
    display_drain();
}

TEST_CASE("zero is dark and is posted", "[kode_sys_display]")
{
    display_fresh();

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_screen_brightness(0U));
    TEST_ASSERT_EQUAL_UINT32(1U, display_count());
    expect_brightness(0U);
    display_drain();
}

TEST_CASE("a level between dark and the lowest lit one posts nothing", "[kode_sys_display]")
{
    display_fresh();

    for ( uint8_t pct = 1U; KODE_SYS_SCREEN_BRIGHT_MIN > pct; ++pct )
    {
        TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_screen_brightness(pct));
    }
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_screen_brightness(KODE_SYS_SCREEN_BRIGHT_MAX + 1U));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_screen_brightness(UINT8_MAX));
    TEST_ASSERT_EQUAL_UINT32(0U, display_count());
}

TEST_CASE("a queue within the margin is KODE_NO_RESOURCES and the event goes back", "[kode_sys_display]")
{
    display_fresh();
    while ( (uint32_t)uxQueueSpacesAvailable(AO_Display->eQueue) > TEST_ROOM_LEFT )
    {
        TEST_ASSERT_TRUE(QACTIVE_POST_X(AO_Display, &s_filler, 0U, NULL));
    }

    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_sys_screen_brightness(TEST_LEVEL));
    TEST_ASSERT_EQUAL_UINT32(TEST_ROOM_LEFT, (uint32_t)uxQueueSpacesAvailable(AO_Display->eQueue));

    display_drain();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_screen_brightness(TEST_LEVEL));
    display_drain();
}
