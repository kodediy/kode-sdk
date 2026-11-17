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

/* ---- The frame object: copied by the accelerator, or drawn by LVGL ----------
 * kscreen_frame_create()'s object is judged at every draw from a few facts
 * about it and about the layer LVGL draws it in. These cases are those facts
 * and what kscreen makes of them; gathering them from LVGL is the board's. */

/**
 * @brief A frame the glass's size, drawn whole into the panel's unturned
 *        framebuffer with nothing else pending: the case the accelerator takes.
 * @return The facts.
 */
static kscreen_geom_frame_t frame_glass(void)
{
    kscreen_geom_frame_t f;

    f.direct = true;
    f.plain  = true;
    f.hooked = false;
    f.idle   = true;
    f.pic_w  = GLASS_W;
    f.pic_h  = GLASS_H;
    f.obj    = (kscreen_geom_area_t){ 0, 0, (int32_t)GLASS_W - 1, (int32_t)GLASS_H - 1 };
    f.clip   = f.obj;
    f.buf    = f.obj;
    return f;
}

TEST_CASE("a frame over the whole glass is copied whole, onto the same place", "[kscreen_geom]")
{
    kscreen_geom_frame_t const f = frame_glass();
    kscreen_geom_block_t from = { 9U, 9U, 9U, 9U };
    kscreen_geom_block_t at   = { 9U, 9U, 9U, 9U };

    TEST_ASSERT_EQUAL_INT(KSCREEN_GEOM_FRAME_COPY, kscreen_geom_frame_judge(&f, &from, &at));
    TEST_ASSERT_EQUAL_UINT16(0U, from.x);
    TEST_ASSERT_EQUAL_UINT16(0U, from.y);
    TEST_ASSERT_EQUAL_UINT16(GLASS_W, from.w);
    TEST_ASSERT_EQUAL_UINT16(GLASS_H, from.h);
    TEST_ASSERT_EQUAL_UINT16(0U, at.x);
    TEST_ASSERT_EQUAL_UINT16(0U, at.y);
    TEST_ASSERT_EQUAL_UINT16(GLASS_W, at.w);
    TEST_ASSERT_EQUAL_UINT16(GLASS_H, at.h);
}

/* With two draw units LVGL draws the glass as two tiles, a DRAW_MAIN each:
 * each is a copy of its own rows, and the two together are the picture. */
TEST_CASE("a tile of the glass is copied from where it falls in the picture", "[kscreen_geom]")
{
    kscreen_geom_frame_t f = frame_glass();
    kscreen_geom_block_t from = { 0U, 0U, 0U, 0U };
    kscreen_geom_block_t at   = { 0U, 0U, 0U, 0U };

    f.clip.y1 = 251;
    TEST_ASSERT_EQUAL_INT(KSCREEN_GEOM_FRAME_COPY, kscreen_geom_frame_judge(&f, &from, &at));
    TEST_ASSERT_EQUAL_UINT16(0U, from.x);
    TEST_ASSERT_EQUAL_UINT16(251U, from.y);
    TEST_ASSERT_EQUAL_UINT16(GLASS_W, from.w);
    TEST_ASSERT_EQUAL_UINT16(GLASS_H - 251U, from.h);
    TEST_ASSERT_EQUAL_UINT16(251U, at.y);
    TEST_ASSERT_EQUAL_UINT16(GLASS_H - 251U, at.h);
}

TEST_CASE("a frame placed inside the glass is copied from its own corner into the buffer's",
          "[kscreen_geom]")
{
    kscreen_geom_frame_t f = frame_glass();
    kscreen_geom_block_t from = { 0U, 0U, 0U, 0U };
    kscreen_geom_block_t at   = { 0U, 0U, 0U, 0U };

    f.pic_w = 100U;
    f.pic_h = 50U;
    f.obj   = (kscreen_geom_area_t){ 10, 20, 109, 69 };
    f.clip  = (kscreen_geom_area_t){ 15, 25, 54, 44 };
    TEST_ASSERT_EQUAL_INT(KSCREEN_GEOM_FRAME_COPY, kscreen_geom_frame_judge(&f, &from, &at));
    TEST_ASSERT_EQUAL_UINT16(5U, from.x);
    TEST_ASSERT_EQUAL_UINT16(5U, from.y);
    TEST_ASSERT_EQUAL_UINT16(40U, from.w);
    TEST_ASSERT_EQUAL_UINT16(20U, from.h);
    TEST_ASSERT_EQUAL_UINT16(15U, at.x);
    TEST_ASSERT_EQUAL_UINT16(25U, at.y);

    /* A buffer that starts lower on the glass takes it that much higher. */
    f.buf = (kscreen_geom_area_t){ 0, 20, (int32_t)GLASS_W - 1, 69 };
    TEST_ASSERT_EQUAL_INT(KSCREEN_GEOM_FRAME_COPY, kscreen_geom_frame_judge(&f, &from, &at));
    TEST_ASSERT_EQUAL_UINT16(15U, at.x);
    TEST_ASSERT_EQUAL_UINT16(5U, at.y);
    TEST_ASSERT_EQUAL_UINT16(40U, at.w);
    TEST_ASSERT_EQUAL_UINT16(20U, at.h);
}

TEST_CASE("a frame the accelerator cannot copy as LVGL would draw it is left to LVGL, and says why",
          "[kscreen_geom]")
{
    kscreen_geom_block_t from = { 0U, 0U, 0U, 0U };
    kscreen_geom_block_t at   = { 0U, 0U, 0U, 0U };
    kscreen_geom_frame_t f;

    f = frame_glass();
    f.direct = false;
    TEST_ASSERT_EQUAL_INT_MESSAGE(KSCREEN_GEOM_FRAME_LAYER, kscreen_geom_frame_judge(&f, &from, &at),
                                  "a band of a turned glass, or a layer of its own");
    f = frame_glass();
    f.plain = false;
    TEST_ASSERT_EQUAL_INT_MESSAGE(KSCREEN_GEOM_FRAME_STYLED, kscreen_geom_frame_judge(&f, &from, &at),
                                  "an opacity, a recolour, a transform, or the app changing the draw");
    f = frame_glass();
    f.hooked = true;
    TEST_ASSERT_EQUAL_INT_MESSAGE(KSCREEN_GEOM_FRAME_HOOKED, kscreen_geom_frame_judge(&f, &from, &at),
                                  "a callback of the app's on DRAW_MAIN, which a copy would end");
    f = frame_glass();
    f.pic_h = (uint16_t)(GLASS_H - 1U);
    TEST_ASSERT_EQUAL_INT_MESSAGE(KSCREEN_GEOM_FRAME_SIZE, kscreen_geom_frame_judge(&f, &from, &at),
                                  "an object resized away from its picture");
    f = frame_glass();
    f.obj.y2 = (int32_t)GLASS_H - 2;
    f.pic_h  = (uint16_t)(GLASS_H - 1U);
    TEST_ASSERT_EQUAL_INT_MESSAGE(KSCREEN_GEOM_FRAME_PART, kscreen_geom_frame_judge(&f, &from, &at),
                                  "an area drawn one row past the object: what is under it shows");
    f = frame_glass();
    f.buf.y2 = 250;
    TEST_ASSERT_EQUAL_INT_MESSAGE(KSCREEN_GEOM_FRAME_PART, kscreen_geom_frame_judge(&f, &from, &at),
                                  "an area drawn past the buffer");
    f = frame_glass();
    f.clip.x1 = 20;
    f.clip.x2 = 19;
    TEST_ASSERT_EQUAL_INT_MESSAGE(KSCREEN_GEOM_FRAME_PART, kscreen_geom_frame_judge(&f, &from, &at),
                                  "an empty area");
    f = frame_glass();
    f.idle = false;
    TEST_ASSERT_EQUAL_INT_MESSAGE(KSCREEN_GEOM_FRAME_BUSY, kscreen_geom_frame_judge(&f, &from, &at),
                                  "LVGL's draw units may still be writing the buffer");
}

/* The fact that changes from one draw to the next is asked last, so a frame
 * that can never be copied says so the same way every time. */
TEST_CASE("a frame refused for two reasons names the lasting one", "[kscreen_geom]")
{
    kscreen_geom_block_t from = { 0U, 0U, 0U, 0U };
    kscreen_geom_block_t at   = { 0U, 0U, 0U, 0U };
    kscreen_geom_frame_t f    = frame_glass();

    f.idle   = false;
    f.direct = false;
    TEST_ASSERT_EQUAL_INT(KSCREEN_GEOM_FRAME_LAYER, kscreen_geom_frame_judge(&f, &from, &at));
    f.direct = true;
    f.plain  = false;
    TEST_ASSERT_EQUAL_INT(KSCREEN_GEOM_FRAME_STYLED, kscreen_geom_frame_judge(&f, &from, &at));
    f.plain  = true;
    f.hooked = true;
    TEST_ASSERT_EQUAL_INT(KSCREEN_GEOM_FRAME_HOOKED, kscreen_geom_frame_judge(&f, &from, &at));
}

TEST_CASE("a refused frame names no block", "[kscreen_geom]")
{
    kscreen_geom_block_t from = { 7U, 7U, 7U, 7U };
    kscreen_geom_block_t at   = { 7U, 7U, 7U, 7U };
    kscreen_geom_frame_t f    = frame_glass();

    f.idle = false;
    TEST_ASSERT_EQUAL_INT(KSCREEN_GEOM_FRAME_BUSY, kscreen_geom_frame_judge(&f, &from, &at));
    TEST_ASSERT_EQUAL_UINT16(7U, from.x);
    TEST_ASSERT_EQUAL_UINT16(7U, from.h);
    TEST_ASSERT_EQUAL_UINT16(7U, at.y);
    TEST_ASSERT_EQUAL_UINT16(7U, at.w);
}

/* LVGL skips everything under an object that covers the area it draws. An
 * image with no background never says it does, so every frame drew the
 * whole screen under the picture first. Covering is about the pixels the
 * object leaves, not about who draws them: a turned glass and a busy LVGL
 * do not change it. */
TEST_CASE("a plain frame of its picture's size covers any area inside it, however it is drawn",
          "[kscreen_geom]")
{
    kscreen_geom_frame_t f = frame_glass();

    TEST_ASSERT_TRUE(kscreen_geom_frame_covers(&f));
    f.direct = false;
    f.idle   = false;
    f.clip   = (kscreen_geom_area_t){ 100, 100, 199, 149 };
    TEST_ASSERT_TRUE_MESSAGE(kscreen_geom_frame_covers(&f), "turned, busy, and a small area");
    f = frame_glass();
    f.hooked = true;
    TEST_ASSERT_TRUE_MESSAGE(kscreen_geom_frame_covers(&f),
                             "a callback of the app's on DRAW_MAIN draws after the picture, over it");

    f = frame_glass();
    f.plain = false;
    TEST_ASSERT_FALSE_MESSAGE(kscreen_geom_frame_covers(&f), "drawn other than as it is");
    f = frame_glass();
    f.pic_w = (uint16_t)(GLASS_W - 1U);
    TEST_ASSERT_FALSE_MESSAGE(kscreen_geom_frame_covers(&f), "an object wider than its picture");
    f = frame_glass();
    f.clip.x1 = -1;
    TEST_ASSERT_FALSE_MESSAGE(kscreen_geom_frame_covers(&f), "an area reaching past its left edge");
}
