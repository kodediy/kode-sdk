/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_kaos_linked.c
 * @brief kaos_linked() with owners a case marks absent.
 */
#include <stddef.h>

#include "fakes.h"
#include "kaos.h"

/** @brief Owners one case may mark absent. */
#define FAKE_ABSENT_MAX 8U

static kaos_start_fn_t s_absent[FAKE_ABSENT_MAX]; /**< The marked starts. */
static size_t          s_n;                       /**< How many.          */

void fake_kaos_absent(kaos_start_fn_t start)
{
    if ( FAKE_ABSENT_MAX > s_n )
    {
        s_absent[s_n] = start;
        ++s_n;
    }
}

void fake_kaos_linked_reset(void)
{
    s_n = 0U;
}

bool kaos_linked(kaos_start_fn_t start)
{
    bool linked = (NULL != start);

    for ( size_t i = 0U; i < s_n; ++i )
    {
        if ( s_absent[i] == start )
        {
            linked = false;
        }
    }
    return linked;
}
