/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kscreen_geom.c
 * @brief What kscreen makes of a composed picture's size, areas, points and turn.
 */
#include <stddef.h>
#include <stdint.h>

#include "unity.h"

#include "kbsp_board.h"
#include "kdrv_compose.h"
#include "kscreen_geom.h"

#define GLASS_W  ((uint16_t)KBSP_LCD_H_RES)
#define GLASS_H  ((uint16_t)KBSP_LCD_V_RES)

TEST_CASE("a picture is allocated in whole cache lines, and only within the limit",
          "[kscreen_geom]")
{
    /* The glass: 411 640 bytes of pixels, 56 past a line. */
    TEST_ASSERT_EQUAL_UINT32(411648U, kscreen_geom_bytes(GLASS_W, GLASS_H));
    TEST_ASSERT_EQUAL_UINT32(0U, kscreen_geom_bytes(GLASS_W, GLASS_H) % KDRV_COMPOSE_ALIGN);
    TEST_ASSERT_EQUAL_UINT32(KDRV_COMPOSE_ALIGN, kscreen_geom_bytes(1U, 1U));
    TEST_ASSERT_EQUAL_UINT32(KDRV_COMPOSE_ALIGN, kscreen_geom_bytes(32U, 1U));
    TEST_ASSERT_EQUAL_UINT32(2U * KDRV_COMPOSE_ALIGN, kscreen_geom_bytes(33U, 1U));

    /* The glass turned is a picture too; a longer side is not. */
    TEST_ASSERT_NOT_EQUAL(0U, kscreen_geom_bytes(GLASS_H, GLASS_W));
    TEST_ASSERT_NOT_EQUAL(0U, kscreen_geom_bytes(GLASS_H, GLASS_H));
    TEST_ASSERT_EQUAL_UINT32(0U, kscreen_geom_bytes((uint16_t)(GLASS_H + 1U), 1U));
    TEST_ASSERT_EQUAL_UINT32(0U, kscreen_geom_bytes(1U, (uint16_t)(GLASS_H + 1U)));
    TEST_ASSERT_EQUAL_UINT32(0U, kscreen_geom_bytes(0U, 8U));
    TEST_ASSERT_EQUAL_UINT32(0U, kscreen_geom_bytes(8U, 0U));
    /* 65535 by 32769: 65 534 bytes when counted in 32 bits. */
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, kscreen_geom_bytes(UINT16_MAX, 32769U),
                                     "a size that wraps is refused, not allocated short");
}

TEST_CASE("an area with both corners inside is a block that counts its last column and row",
          "[kscreen_geom]")
{
    kscreen_geom_block_t blk  = { 9U, 9U, 9U, 9U };
    kscreen_geom_area_t  area = { 0, 0, 0, 0 };

    TEST_ASSERT_TRUE(kscreen_geom_block(32U, 8U, NULL, &blk));
    TEST_ASSERT_EQUAL_UINT16(0U, blk.x);
    TEST_ASSERT_EQUAL_UINT16(0U, blk.y);
    TEST_ASSERT_EQUAL_UINT16(32U, blk.w);
    TEST_ASSERT_EQUAL_UINT16(8U, blk.h);

    TEST_ASSERT_TRUE_MESSAGE(kscreen_geom_block(32U, 8U, &area, &blk), "one pixel is an area");
    TEST_ASSERT_EQUAL_UINT16(1U, blk.w);
    TEST_ASSERT_EQUAL_UINT16(1U, blk.h);

    area = (kscreen_geom_area_t){ 4, 2, 31, 7 };
    TEST_ASSERT_TRUE(kscreen_geom_block(32U, 8U, &area, &blk));
    TEST_ASSERT_EQUAL_UINT16(4U, blk.x);
    TEST_ASSERT_EQUAL_UINT16(2U, blk.y);
    TEST_ASSERT_EQUAL_UINT16(28U, blk.w);
    TEST_ASSERT_EQUAL_UINT16(6U, blk.h);

    area.x2 = 32;
    TEST_ASSERT_FALSE_MESSAGE(kscreen_geom_block(32U, 8U, &area, &blk), "a column past the last");
    area = (kscreen_geom_area_t){ 4, 2, 31, 8 };
    TEST_ASSERT_FALSE_MESSAGE(kscreen_geom_block(32U, 8U, &area, &blk), "a row past the last");
    area = (kscreen_geom_area_t){ -1, 0, 3, 3 };
    TEST_ASSERT_FALSE_MESSAGE(kscreen_geom_block(32U, 8U, &area, &blk), "left of the picture");
    area = (kscreen_geom_area_t){ 5, 0, 4, 3 };
    TEST_ASSERT_FALSE_MESSAGE(kscreen_geom_block(32U, 8U, &area, &blk), "corners the wrong way");
}

TEST_CASE("a landing point is a pixel of the picture", "[kscreen_geom]")
{
    TEST_ASSERT_TRUE(kscreen_geom_point(32U, 8U, 0, 0));
    TEST_ASSERT_TRUE(kscreen_geom_point(32U, 8U, 31, 7));
    TEST_ASSERT_FALSE(kscreen_geom_point(32U, 8U, 32, 0));
    TEST_ASSERT_FALSE(kscreen_geom_point(32U, 8U, 0, 8));
    TEST_ASSERT_FALSE(kscreen_geom_point(32U, 8U, -1, 0));
    TEST_ASSERT_FALSE(kscreen_geom_point(32U, 8U, 0, -1));
}

/* A glass turned by a quarter is drawn into the panel's memory turned that
 * quarter, counter-clockwise. The copy of the glass turns the other way by
 * as much, and lands in a picture with the glass's sides as they are seen. */
TEST_CASE("the copy of a turned glass is turned back, into a picture of the turned size",
          "[kscreen_geom]")
{
    TEST_ASSERT_EQUAL_UINT8(0U, kscreen_geom_grab_turns(0U));
    TEST_ASSERT_EQUAL_UINT8(3U, kscreen_geom_grab_turns(1U));
    TEST_ASSERT_EQUAL_UINT8(2U, kscreen_geom_grab_turns(2U));
    TEST_ASSERT_EQUAL_UINT8(1U, kscreen_geom_grab_turns(3U));
    for ( uint8_t rot = 0U; KSCREEN_GEOM_TURNS > rot; rot++ )
    {
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(0U, (uint8_t)((rot + kscreen_geom_grab_turns(rot))
                                                      % KSCREEN_GEOM_TURNS),
                                        "the glass's turn and the copy's make a whole one");
    }

    TEST_ASSERT_TRUE(kscreen_geom_grab_fits(0U, GLASS_W, GLASS_H));
    TEST_ASSERT_TRUE(kscreen_geom_grab_fits(2U, GLASS_W, GLASS_H));
    TEST_ASSERT_FALSE(kscreen_geom_grab_fits(0U, GLASS_H, GLASS_W));
    TEST_ASSERT_TRUE(kscreen_geom_grab_fits(1U, GLASS_H, GLASS_W));
    TEST_ASSERT_TRUE(kscreen_geom_grab_fits(3U, GLASS_H, GLASS_W));
    TEST_ASSERT_FALSE_MESSAGE(kscreen_geom_grab_fits(1U, GLASS_W, GLASS_H),
                              "a quarter turn swaps the sides the picture must have");
    TEST_ASSERT_FALSE(kscreen_geom_grab_fits(0U, GLASS_W, (uint16_t)(GLASS_H - 1U)));
}

/* LVGL may reshape a draw buffer so that a row carries padding after its
 * pixels. The accelerator reads rows back to back, so such a picture would
 * come out sheared: it is told apart here, by its stride. */
TEST_CASE("a picture whose rows carry padding is not one the accelerator reads", "[kscreen_geom]")
{
    TEST_ASSERT_TRUE(kscreen_geom_packed(GLASS_W, (uint32_t)GLASS_W * KDRV_COMPOSE_PX_BYTES));
    TEST_ASSERT_TRUE(kscreen_geom_packed(1U, KDRV_COMPOSE_PX_BYTES));
    TEST_ASSERT_FALSE(kscreen_geom_packed(GLASS_W, ((uint32_t)GLASS_W * KDRV_COMPOSE_PX_BYTES) + 4U));
    TEST_ASSERT_FALSE(kscreen_geom_packed(GLASS_W, (uint32_t)GLASS_W));
}

/* A dim paints its destination black before it lays the source over it. Given
 * one picture twice it would blacken the only copy and then refuse, so the
 * pair is judged before anything is painted. */
TEST_CASE("a dim takes two pictures of one size, never one picture twice", "[kscreen_geom]")
{
    static uint8_t a[4];
    static uint8_t b[4];

    TEST_ASSERT_TRUE(kscreen_geom_pair(a, b, GLASS_W, GLASS_H, GLASS_W, GLASS_H));
    TEST_ASSERT_FALSE_MESSAGE(kscreen_geom_pair(a, a, GLASS_W, GLASS_H, GLASS_W, GLASS_H),
                              "the same picture as source and destination");
    TEST_ASSERT_FALSE(kscreen_geom_pair(a, b, GLASS_W, GLASS_H, GLASS_H, GLASS_W));
    TEST_ASSERT_FALSE(kscreen_geom_pair(a, b, GLASS_W, GLASS_H, GLASS_W, (uint16_t)(GLASS_H - 1U)));
}
