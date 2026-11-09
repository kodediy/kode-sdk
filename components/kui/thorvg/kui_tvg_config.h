/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kui_tvg_config.h
 * @brief ThorVG's build config for kui: LVGL 9.6's own, without the SVG loader or ThorVG's threads.
 */
/* Takes the guard of LVGL's src/libs/thorvg/config.h, which then compiles empty. Diff the two
 * on every LVGL bump: this is config.h less the two lines named below. */
#ifndef TVG_CONFIG_H
#define TVG_CONFIG_H

#include "lvgl_public.h"

#define THORVG_SW_RASTER_SUPPORT 1

#define THORVG_LOTTIE_LOADER_SUPPORT LV_USE_LOTTIE

/* Left out: THORVG_SVG_LOADER_SUPPORT (std::ifstream, about 200 KB of flash) and
 * THORVG_THREAD_SUPPORT (3 KB pthread stacks, which the rasteriser overflows). */
#define THORVG_VERSION_STRING "0.15.3"


#endif /* TVG_CONFIG_H */
