/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_touch.c
 * @brief The standard layer's finger: kode_touch_read() and the names of the three touch events.
 */
#include "kode.h"

#include "kode_entry.h"
#include "kode_standard_ao.h"
#include "kaos.h"

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
const char *kode_event_touch_pressed(void)
{
    return KODE_EVENT_TOUCH_PRESSED_NAME;
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
const char *kode_event_touch_moved(void)
{
    return KODE_EVENT_TOUCH_MOVED_NAME;
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
const char *kode_event_touch_released(void)
{
    return KODE_EVENT_TOUCH_RELEASED_NAME;
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
bool kode_touch_read(int16_t *x, int16_t *y)
{
    bool down = false;

    if ( kode_enter("kode_touch_read") && kode_standard_ao_ensure() )
    {
        if ( kode_standard_ao_touch_listen(&touch_subscribe) )
        {
            down = kode_event_touch_read(x, y);
        }
        else
        {
            kode_err_set("the touch panel cannot be heard now: the standard layer is busy, try again");
        }
    }
    return down;
}
