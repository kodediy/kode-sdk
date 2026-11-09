/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kbsp_station.c
 * @brief The E5 station's facts: the camera module's four pin maps on J3.
 */
#include <stdbool.h>
#include <stdint.h>

#include "unity.h"

#include "kbsp_exp.h"
#include "kbsp_pins.h"
#include "kbsp_station.h"

/** @brief The fourteen lines of @p m, in the order the struct names them. */
static void lines_of(const kbsp_cam_map_t *m, uint8_t out[KBSP_CAM_LINES])
{
    out[0] = m->scl;
    out[1] = m->sda;
    out[2] = m->pwdn;
    out[3] = m->vsync;
    out[4] = m->href;
    out[5] = m->pclk;
    for ( uint32_t i = 0U; KBSP_CAM_DATA_LINES > i; ++i )
    {
        out[6U + i] = m->d[i];
    }
}

TEST_CASE("each camera map takes J3's fourteen positions once each", "[kbsp_station]")
{
    TEST_ASSERT_EQUAL_UINT32(KBSP_EXP_COUNT, KBSP_CAM_LINES);

    for ( uint32_t m = 0U; KBSP_CAM_MAP_COUNT > m; ++m )
    {
        uint8_t lines[KBSP_CAM_LINES];
        bool    seen[KBSP_EXP_COUNT + 1U] = { false };

        lines_of(&KBSP_CAM_MAPS[m], lines);
        for ( uint32_t i = 0U; KBSP_CAM_LINES > i; ++i )
        {
            TEST_ASSERT_TRUE(1U <= lines[i]);
            TEST_ASSERT_TRUE(KBSP_EXP_COUNT >= lines[i]);
            TEST_ASSERT_FALSE_MESSAGE(seen[lines[i]], "a map names a position twice");
            seen[lines[i]] = true;
        }
    }
}

TEST_CASE("a version's turned map is its upright map turned half round", "[kbsp_station]")
{
    static const uint32_t pairs[][2] = {
        { KBSP_CAM_MAP_V10_UPRIGHT, KBSP_CAM_MAP_V10_TURNED },
        { KBSP_CAM_MAP_V11_UPRIGHT, KBSP_CAM_MAP_V11_TURNED },
    };

    for ( uint32_t p = 0U; (sizeof(pairs) / sizeof(pairs[0])) > p; ++p )
    {
        uint8_t up[KBSP_CAM_LINES];
        uint8_t turned[KBSP_CAM_LINES];

        lines_of(&KBSP_CAM_MAPS[pairs[p][0]], up);
        lines_of(&KBSP_CAM_MAPS[pairs[p][1]], turned);
        for ( uint32_t i = 0U; KBSP_CAM_LINES > i; ++i )
        {
            TEST_ASSERT_EQUAL_UINT8((uint8_t)(KBSP_EXP_COUNT + 1U - up[i]), turned[i]);
        }
    }
}

TEST_CASE("the upright maps share one I2C pair, GPIO8/9, and the turned ones GPIO16/15",
          "[kbsp_station]")
{
    static const uint32_t upright[] = { KBSP_CAM_MAP_V10_UPRIGHT, KBSP_CAM_MAP_V11_UPRIGHT };
    static const uint32_t turned[]  = { KBSP_CAM_MAP_V10_TURNED, KBSP_CAM_MAP_V11_TURNED };

    for ( uint32_t i = 0U; KBSP_CAM_VERSIONS > i; ++i )
    {
        const kbsp_cam_map_t *const u = &KBSP_CAM_MAPS[upright[i]];
        const kbsp_cam_map_t *const t = &KBSP_CAM_MAPS[turned[i]];

        TEST_ASSERT_EQUAL_UINT8(KBSP_PIN_EXP_1, KBSP_EXP[u->scl - 1U].gpio);
        TEST_ASSERT_EQUAL_UINT8(KBSP_PIN_EXP_2, KBSP_EXP[u->sda - 1U].gpio);
        TEST_ASSERT_EQUAL_UINT8(KBSP_PIN_EXP_14, KBSP_EXP[t->scl - 1U].gpio);
        TEST_ASSERT_EQUAL_UINT8(KBSP_PIN_EXP_13, KBSP_EXP[t->sda - 1U].gpio);
    }
}

TEST_CASE("the two versions differ in where PWDN sits", "[kbsp_station]")
{
    TEST_ASSERT_NOT_EQUAL(KBSP_CAM_MAPS[KBSP_CAM_MAP_V10_UPRIGHT].pwdn,
                          KBSP_CAM_MAPS[KBSP_CAM_MAP_V11_UPRIGHT].pwdn);
    TEST_ASSERT_NOT_EQUAL(KBSP_CAM_MAPS[KBSP_CAM_MAP_V10_TURNED].pwdn,
                          KBSP_CAM_MAPS[KBSP_CAM_MAP_V11_TURNED].pwdn);
}
