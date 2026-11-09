/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_screen_bright.c
 * @brief The standard layer's screen brightness: kode_display_set_brightness().
 */
#include "kode.h"

#include "kode_entry.h"
#include "kode_sys.h"

_Static_assert(KODE_DISPLAY_BRIGHTNESS_MIN == KODE_SYS_SCREEN_BRIGHT_MIN,
               "the lowest lit brightness is the system's");
_Static_assert(KODE_DISPLAY_BRIGHTNESS_MAX == KODE_SYS_SCREEN_BRIGHT_MAX,
               "the highest brightness is the system's");

/** @brief How long the call keeps trying when the system is busy, in ms. */
#define KODE_DISPLAY_BRIGHTNESS_BOUND_MS 100U

/** @brief The text kode_error() gives when AO_Display never took the level. */
#define KODE_DISPLAY_BRIGHTNESS_REFUSED "the screen did not take the brightness"

/** @brief One try: true when AO_Display's event was posted. */
/* cppcheck-suppress constParameterCallback ; the signature is the one kode_retry() takes */
static bool bright_attempt(void *ctx)
{
    /* cppcheck-suppress misra-c2012-11.5 ; the context is the uint8_t kode_display_set_brightness() passed */
    const uint8_t *const pct = (const uint8_t *)ctx;

    return kode_sys_screen_brightness(*pct) == KODE_OK;
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
bool kode_display_set_brightness(uint8_t pct)
{
    bool taken = false;

    if ( kode_enter("kode_display_set_brightness") )
    {
        uint8_t level = pct;

        if ( (0U != pct) && ((KODE_DISPLAY_BRIGHTNESS_MIN > pct) || (KODE_DISPLAY_BRIGHTNESS_MAX < pct)) )
        {
            kode_err_set("brightness out of range: use 0, or %u to %u",
                         (unsigned)KODE_DISPLAY_BRIGHTNESS_MIN, (unsigned)KODE_DISPLAY_BRIGHTNESS_MAX);
        }
        else
        {
            taken = kode_retry(&bright_attempt, &level, KODE_DISPLAY_BRIGHTNESS_BOUND_MS);
            if ( !taken )
            {
                kode_err_set(KODE_DISPLAY_BRIGHTNESS_REFUSED);
            }
        }
    }
    return taken;
}
