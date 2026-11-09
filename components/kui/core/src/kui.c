/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kui.c
 * @brief kui_init(): the theme, then the shared styles.
 */
#include <stdbool.h>

#include "kui.h"
#include "kui_internal.h"

/** Set once kui_init() has run: a second call does nothing. */
static bool s_ready = false;

void kui_init(lv_display_t *disp)
{
    if ( s_ready )
    {
        return;
    }
    /* The simple theme under every widget, as the glass has always had it:
     * without it every default the theme sets (a button's radius, a label's
     * colour before a style lands) changes under the screens at once. */
    lv_display_set_theme(disp, lv_theme_simple_init(disp));
    kui_styles_init();
    s_ready = true;
}
