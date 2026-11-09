/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kaos_linked.c
 * @brief kaos_linked(): whether an owner named weakly is in the image.
 */
#include "kaos.h"

/* Out of line, so a static analyser cannot fold the test of a weak reference to always true,
 * and the host suite can stand in for it. */
bool kaos_linked(kaos_start_fn_t start)
{
    return NULL != start;
}
