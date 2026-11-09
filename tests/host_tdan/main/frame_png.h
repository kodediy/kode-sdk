/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file frame_png.h
 * @brief Reads a frame of the suite: an 8-bit RGB PNG, returned as the camera's RGB565.
 */
#ifndef FRAME_PNG_H_
#define FRAME_PNG_H_

#include <stdint.h>

/**
 * @brief Load a PNG (8-bit RGB, not interlaced) as RGB565.
 * @param path The file.
 * @param w    Width out.
 * @param h    Height out.
 * @return The pixels (free() them), or NULL when the file cannot be read.
 */
uint16_t *frame_png_load(const char *path, int32_t *w, int32_t *h);

#endif /* FRAME_PNG_H_ */
