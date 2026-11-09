/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kbsp_station.c
 * @brief The camera module's four pin maps, as J3 positions (GPIO in the comments).
 */
#include "kbsp_station.h"

const kbsp_cam_map_t KBSP_CAM_MAPS[] = {
    [KBSP_CAM_MAP_V10_UPRIGHT] = {
        .scl = 1U, .sda = 2U, .pwdn = 4U, .vsync = 3U, .href = 5U, .pclk = 9U,
        /* GPIO 8, 9, 11, 10, 12, 19 */
        .d = { 11U, 13U, 14U, 12U, 10U, 8U, 7U, 6U },
        /* GPIO 17, 15, 16, 18, 20, 21, 14, 13 */
    },
    [KBSP_CAM_MAP_V10_TURNED] = {
        .scl = 14U, .sda = 13U, .pwdn = 11U, .vsync = 12U, .href = 10U, .pclk = 6U,
        /* GPIO 16, 15, 17, 18, 20, 13 */
        .d = { 4U, 2U, 1U, 3U, 5U, 7U, 8U, 9U },
        /* GPIO 11, 9, 8, 10, 12, 14, 21, 19 */
    },
    [KBSP_CAM_MAP_V11_UPRIGHT] = {
        .scl = 1U, .sda = 2U, .pwdn = 13U, .vsync = 14U, .href = 12U, .pclk = 8U,
        /* GPIO 8, 9, 15, 16, 18, 21 */
        .d = { 6U, 4U, 3U, 5U, 7U, 9U, 10U, 11U },
        /* GPIO 13, 11, 10, 12, 14, 19, 20, 17 */
    },
    [KBSP_CAM_MAP_V11_TURNED] = {
        .scl = 14U, .sda = 13U, .pwdn = 2U, .vsync = 1U, .href = 3U, .pclk = 7U,
        /* GPIO 16, 15, 9, 8, 10, 14 */
        .d = { 9U, 11U, 12U, 10U, 8U, 6U, 5U, 4U },
        /* GPIO 19, 17, 18, 20, 21, 13, 12, 11 */
    },
};

_Static_assert((sizeof(KBSP_CAM_MAPS) / sizeof(KBSP_CAM_MAPS[0])) == KBSP_CAM_MAP_COUNT,
               "one map per version and side");
