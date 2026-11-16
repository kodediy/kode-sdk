/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kdrv_compose_check.c
 * @brief What the accelerator's door accepts and what it refuses, with no accelerator.
 */
#include <stddef.h>
#include <stdint.h>

#include "unity.h"

#include "kdrv_compose.h"
#include "kdrv_compose_check.h"

#define PIC_W      32U   /* a small picture: 32 by 8 is 512 bytes, eight cache lines */
#define PIC_H      8U
#define PIC_BYTES  (PIC_W * PIC_H * KDRV_COMPOSE_PX_BYTES)

static uint8_t s_a[PIC_BYTES] __attribute__((aligned(KDRV_COMPOSE_ALIGN)));
static uint8_t s_b[PIC_BYTES] __attribute__((aligned(KDRV_COMPOSE_ALIGN)));

static kdrv_compose_pic_t pic_of(uint8_t *px)
{
    kdrv_compose_pic_t const p = { .px = px, .bytes = PIC_BYTES, .w = (uint16_t)PIC_W,
                                   .h = (uint16_t)PIC_H };
    return p;
}

TEST_CASE("a scaled length is counted in sixteenths, rounded down", "[kdrv_compose]")
{
    TEST_ASSERT_EQUAL_UINT32(410U, kdrv_compose_scaled(410U, KDRV_COMPOSE_SCALE_ONE));
    TEST_ASSERT_EQUAL_UINT32(205U, kdrv_compose_scaled(410U, 8U));
    TEST_ASSERT_EQUAL_UINT32(251U, kdrv_compose_scaled(502U, 8U));
    TEST_ASSERT_EQUAL_UINT32(384U, kdrv_compose_scaled(410U, 15U));
    TEST_ASSERT_EQUAL_UINT32(615U, kdrv_compose_scaled(410U, 24U));
    TEST_ASSERT_EQUAL_UINT32(0U, kdrv_compose_scaled(15U, KDRV_COMPOSE_SCALE_MIN));
}

TEST_CASE("a fill needs a whole-cache-line picture and a block inside it", "[kdrv_compose]")
{
    kdrv_compose_pic_t  dst  = pic_of(s_a);
    kdrv_compose_rect_t at   = { 0U, 0U, (uint16_t)PIC_W, (uint16_t)PIC_H };

    TEST_ASSERT_EQUAL(KODE_OK, kdrv_compose_check_fill(&dst, &at));
    TEST_ASSERT_EQUAL(KODE_DENIED, kdrv_compose_check_fill(NULL, &at));
    TEST_ASSERT_EQUAL(KODE_DENIED, kdrv_compose_check_fill(&dst, NULL));

    /* The last pixel alone is a block; one past it is not. */
    at = (kdrv_compose_rect_t){ (uint16_t)(PIC_W - 1U), (uint16_t)(PIC_H - 1U), 1U, 1U };
    TEST_ASSERT_EQUAL(KODE_OK, kdrv_compose_check_fill(&dst, &at));
    at.w = 2U;
    TEST_ASSERT_EQUAL_MESSAGE(KODE_DENIED, kdrv_compose_check_fill(&dst, &at), "past the right edge");
    at = (kdrv_compose_rect_t){ 0U, (uint16_t)PIC_H, 1U, 1U };
    TEST_ASSERT_EQUAL_MESSAGE(KODE_DENIED, kdrv_compose_check_fill(&dst, &at), "below the last row");
    at = (kdrv_compose_rect_t){ 0U, 0U, 0U, 1U };
    TEST_ASSERT_EQUAL_MESSAGE(KODE_DENIED, kdrv_compose_check_fill(&dst, &at), "an empty block");

    at = (kdrv_compose_rect_t){ 0U, 0U, 1U, 1U };
    dst.px = NULL;
    TEST_ASSERT_EQUAL_MESSAGE(KODE_DENIED, kdrv_compose_check_fill(&dst, &at), "no pixels");
    dst = pic_of(&s_a[2]);
    TEST_ASSERT_EQUAL_MESSAGE(KODE_DENIED, kdrv_compose_check_fill(&dst, &at),
                              "an address off the cache line");
    dst = pic_of(s_a);
    dst.h = (uint16_t)(PIC_H - 1U);
    dst.bytes = PIC_BYTES - KDRV_COMPOSE_PX_BYTES;
    TEST_ASSERT_EQUAL_MESSAGE(KODE_DENIED, kdrv_compose_check_fill(&dst, &at),
                              "a size that is not whole cache lines");
    dst = pic_of(s_a);
    dst.bytes = PIC_BYTES - KDRV_COMPOSE_ALIGN;
    TEST_ASSERT_EQUAL_MESSAGE(KODE_DENIED, kdrv_compose_check_fill(&dst, &at),
                              "less room than the picture");
}

TEST_CASE("a scale must be in range and its result must fit the destination", "[kdrv_compose]")
{
    kdrv_compose_pic_t const  dst  = pic_of(s_a);
    kdrv_compose_pic_t        src  = pic_of(s_b);
    kdrv_compose_rect_t       from = { 0U, 0U, (uint16_t)PIC_W, (uint16_t)PIC_H };

    TEST_ASSERT_EQUAL(KODE_OK, kdrv_compose_check_scale(&dst, 0U, 0U, &src, &from,
                                                        KDRV_COMPOSE_SCALE_ONE,
                                                        KDRV_COMPOSE_SCALE_ONE, 0U));
    /* Halved, it fits anywhere its 16 by 4 stays inside. */
    TEST_ASSERT_EQUAL(KODE_OK, kdrv_compose_check_scale(&dst, 16U, 4U, &src, &from, 8U, 8U, 0U));
    TEST_ASSERT_EQUAL_MESSAGE(KODE_DENIED,
                              kdrv_compose_check_scale(&dst, 17U, 4U, &src, &from, 8U, 8U, 0U),
                              "a column too far right");
    TEST_ASSERT_EQUAL_MESSAGE(KODE_DENIED,
                              kdrv_compose_check_scale(&dst, 0U, 0U, &src, &from, 17U,
                                                       KDRV_COMPOSE_SCALE_ONE, 0U),
                              "wider than the destination");
    TEST_ASSERT_EQUAL_MESSAGE(KODE_DENIED,
                              kdrv_compose_check_scale(&dst, 0U, 0U, &src, &from, 0U,
                                                       KDRV_COMPOSE_SCALE_ONE, 0U),
                              "a scale of nothing");
    TEST_ASSERT_EQUAL_MESSAGE(KODE_DENIED,
                              kdrv_compose_check_scale(&dst, 0U, 0U, &src, &from,
                                                       KDRV_COMPOSE_SCALE_ONE,
                                                       (uint16_t)(KDRV_COMPOSE_SCALE_MAX + 1U), 0U),
                              "past sixteen times");
    /* Eight rows at one sixteenth are no row at all. */
    TEST_ASSERT_EQUAL_MESSAGE(KODE_DENIED,
                              kdrv_compose_check_scale(&dst, 0U, 0U, &src, &from,
                                                       KDRV_COMPOSE_SCALE_ONE,
                                                       KDRV_COMPOSE_SCALE_MIN, 0U),
                              "scaled to nothing");
    /* A source block of 2 by 1 grows sixteen times to 32 by 16: too tall. */
    from = (kdrv_compose_rect_t){ 0U, 0U, 2U, 1U };
    TEST_ASSERT_EQUAL(KODE_DENIED, kdrv_compose_check_scale(&dst, 0U, 0U, &src, &from,
                                                            KDRV_COMPOSE_SCALE_MAX,
                                                            KDRV_COMPOSE_SCALE_MAX, 0U));
    TEST_ASSERT_EQUAL(KODE_OK, kdrv_compose_check_scale(&dst, 0U, 0U, &src, &from,
                                                        KDRV_COMPOSE_SCALE_MAX, 128U, 0U));
    from = (kdrv_compose_rect_t){ 1U, 0U, (uint16_t)PIC_W, 1U };
    TEST_ASSERT_EQUAL_MESSAGE(KODE_DENIED,
                              kdrv_compose_check_scale(&dst, 0U, 0U, &src, &from,
                                                       KDRV_COMPOSE_SCALE_ONE,
                                                       KDRV_COMPOSE_SCALE_ONE, 0U),
                              "a source block past its picture");

    /* A source needs no alignment; a picture is never scaled into itself. */
    from = (kdrv_compose_rect_t){ 0U, 0U, 4U, 4U };
    src.px = &s_b[2];
    src.bytes = PIC_BYTES - 2U;
    src.h = (uint16_t)(PIC_H - 1U);
    TEST_ASSERT_EQUAL(KODE_OK, kdrv_compose_check_scale(&dst, 0U, 0U, &src, &from,
                                                        KDRV_COMPOSE_SCALE_ONE,
                                                        KDRV_COMPOSE_SCALE_ONE, 0U));
    TEST_ASSERT_EQUAL_MESSAGE(KODE_DENIED,
                              kdrv_compose_check_scale(&dst, 0U, 0U, &dst, &from,
                                                       KDRV_COMPOSE_SCALE_ONE,
                                                       KDRV_COMPOSE_SCALE_ONE, 0U),
                              "onto itself");
    TEST_ASSERT_EQUAL(KODE_DENIED, kdrv_compose_check_scale(&dst, 0U, 0U, &src, NULL,
                                                            KDRV_COMPOSE_SCALE_ONE,
                                                            KDRV_COMPOSE_SCALE_ONE, 0U));
    TEST_ASSERT_EQUAL(KODE_DENIED, kdrv_compose_check_scale(&dst, 0U, 0U, NULL, &from,
                                                            KDRV_COMPOSE_SCALE_ONE,
                                                            KDRV_COMPOSE_SCALE_ONE, 0U));
}

TEST_CASE("a blend lands a block of another picture inside the destination", "[kdrv_compose]")
{
    kdrv_compose_pic_t const  dst  = pic_of(s_a);
    kdrv_compose_pic_t const  fg   = pic_of(s_b);
    kdrv_compose_rect_t       from = { 0U, 0U, (uint16_t)PIC_W, (uint16_t)PIC_H };

    TEST_ASSERT_EQUAL(KODE_OK, kdrv_compose_check_blend(&dst, 0U, 0U, &fg, &from));
    TEST_ASSERT_EQUAL_MESSAGE(KODE_DENIED, kdrv_compose_check_blend(&dst, 1U, 0U, &fg, &from),
                              "pushed past the right edge");
    from = (kdrv_compose_rect_t){ 8U, 2U, 8U, 2U };
    TEST_ASSERT_EQUAL(KODE_OK, kdrv_compose_check_blend(&dst, 24U, 6U, &fg, &from));
    TEST_ASSERT_EQUAL_MESSAGE(KODE_DENIED, kdrv_compose_check_blend(&dst, 24U, 7U, &fg, &from),
                              "pushed below the last row");
    TEST_ASSERT_EQUAL_MESSAGE(KODE_DENIED, kdrv_compose_check_blend(&dst, 0U, 0U, &dst, &from),
                              "over itself");
    TEST_ASSERT_EQUAL(KODE_DENIED, kdrv_compose_check_blend(&dst, 0U, 0U, &fg, NULL));
    TEST_ASSERT_EQUAL(KODE_DENIED, kdrv_compose_check_blend(NULL, 0U, 0U, &fg, &from));
}

/* A picture's sides are 16 bits each: 65535 by 32769 pixels of two bytes are
 * 4 295 032 830 bytes, which 32 bits read as 65 534. Counted so, the picture
 * below had room; it is thousands of times its allocation. */
TEST_CASE("a picture too large to count in 32 bits is refused, not wrapped", "[kdrv_compose]")
{
    kdrv_compose_pic_t        huge = pic_of(s_a);
    kdrv_compose_pic_t const  src  = pic_of(s_b);
    kdrv_compose_rect_t const one  = { 0U, 0U, 1U, 1U };

    huge.w     = UINT16_MAX;
    huge.h     = 32769U;
    huge.bytes = 65536U;
    TEST_ASSERT_EQUAL(KODE_DENIED, kdrv_compose_check_fill(&huge, &one));
    TEST_ASSERT_EQUAL(KODE_DENIED, kdrv_compose_check_scale(&huge, 0U, 0U, &src, &one,
                                                            KDRV_COMPOSE_SCALE_ONE,
                                                            KDRV_COMPOSE_SCALE_ONE, 0U));
    TEST_ASSERT_EQUAL_MESSAGE(KODE_DENIED, kdrv_compose_check_blend(&src, 0U, 0U, &huge, &one),
                              "as a source too");
}

/* The limit is the glass's longer side: a picture is at most a little over a
 * full frame, which bounds the pass AO_Display runs inside one step. */
TEST_CASE("no side of a picture is longer than the glass's longer one", "[kdrv_compose]")
{
    static uint8_t            room[8U * KDRV_COMPOSE_ALIGN * 32U]
        __attribute__((aligned(KDRV_COMPOSE_ALIGN)));
    kdrv_compose_pic_t        wide = { room, (uint32_t)sizeof(room),
                                       (uint16_t)KDRV_COMPOSE_SIDE_MAX, 2U };
    kdrv_compose_rect_t const one  = { 0U, 0U, 1U, 1U };

    TEST_ASSERT_EQUAL(KODE_OK, kdrv_compose_check_fill(&wide, &one));
    wide.w = (uint16_t)(KDRV_COMPOSE_SIDE_MAX + 1U);
    TEST_ASSERT_EQUAL_MESSAGE(KODE_DENIED, kdrv_compose_check_fill(&wide, &one), "too wide");
    wide.w = 2U;
    wide.h = (uint16_t)(KDRV_COMPOSE_SIDE_MAX + 1U);
    TEST_ASSERT_EQUAL_MESSAGE(KODE_DENIED, kdrv_compose_check_fill(&wide, &one), "too tall");
}

/* A quarter turn lands a block as tall as it was wide: 32 by 8 becomes 8 by
 * 32, which a picture of 32 by 8 cannot take, and 8 by 8 stays 8 by 8. */
TEST_CASE("a turned block must fit by the sides it lands with", "[kdrv_compose]")
{
    kdrv_compose_pic_t const  dst  = pic_of(s_a);
    kdrv_compose_pic_t const  src  = pic_of(s_b);
    kdrv_compose_rect_t       from = { 0U, 0U, (uint16_t)PIC_W, (uint16_t)PIC_H };
    uint32_t                  w    = 0U;
    uint32_t                  h    = 0U;

    kdrv_compose_landed(&from, KDRV_COMPOSE_SCALE_ONE, 8U, 0U, &w, &h);
    TEST_ASSERT_EQUAL_UINT32(PIC_W, w);
    TEST_ASSERT_EQUAL_UINT32(PIC_H / 2U, h);
    kdrv_compose_landed(&from, KDRV_COMPOSE_SCALE_ONE, 8U, 3U, &w, &h);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(PIC_H / 2U, w, "its halved height is its width now");
    TEST_ASSERT_EQUAL_UINT32(PIC_W, h);

    for ( uint8_t turns = 0U; KDRV_COMPOSE_TURNS > turns; turns++ )
    {
        kode_status_t want = KODE_OK;

        if ( 0U != (turns % 2U) )
        {
            want = KODE_DENIED;
        }
        TEST_ASSERT_EQUAL(want, kdrv_compose_check_scale(&dst, 0U, 0U, &src, &from,
                                                         KDRV_COMPOSE_SCALE_ONE,
                                                         KDRV_COMPOSE_SCALE_ONE, turns));
    }
    from = (kdrv_compose_rect_t){ 0U, 0U, (uint16_t)PIC_H, (uint16_t)PIC_H };
    TEST_ASSERT_EQUAL(KODE_OK, kdrv_compose_check_scale(&dst, 24U, 0U, &src, &from,
                                                        KDRV_COMPOSE_SCALE_ONE,
                                                        KDRV_COMPOSE_SCALE_ONE, 1U));
    TEST_ASSERT_EQUAL_MESSAGE(KODE_DENIED,
                              kdrv_compose_check_scale(&dst, 0U, 0U, &src, &from,
                                                       KDRV_COMPOSE_SCALE_ONE,
                                                       KDRV_COMPOSE_SCALE_ONE,
                                                       (uint8_t)KDRV_COMPOSE_TURNS),
                              "four quarter turns is not a turn");
}
