/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kui_internal.h
 * @brief What kui's own files share and an app never sees.
 */
#ifndef KUI_INTERNAL_H
#define KUI_INTERNAL_H

#include <stdint.h>

/** @brief Initialise every kui_style_*; called once, from kui_init(). */
void kui_styles_init(void);

/**
 * @brief floor(sqrt(n)) for n >= 0, by Newton's method on integers.
 * @param n The radicand.
 * @return The largest x with x * x <= n.
 */
int32_t kui_isqrt_floor(int64_t n);

#endif /* KUI_INTERNAL_H */
