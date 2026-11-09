/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file minimal.c
 * @brief The minimal app (variant `minimal`): the LED and the buttons, polled and as events.
 */
#include "kode.h"

/** @brief How long the loop sleeps between two looks at the buttons, in ms. */
#define MINIMAL_POLL_MS 50U

/** @brief The LED's level when a button is down. */
#define MINIMAL_LEVEL   32U

/**
 * @brief The bottom button's press, as an event: the event path, which starts
 *        the standard layer's own Active Object.
 * @param e The event.
 */
static void on_bottom(const kode_event_t *e)
{
    (void)e;
    (void)kode_led_set_rgb(MINIMAL_LEVEL, 0U, 0U);
}

void app_main(void)
{
    kode_init();
    (void)kode_event_add_listener("button_bottom_pressed", on_bottom);
    for ( ;; )
    {
        kode_buttons_t const pad = kode_button_read_all();
        uint8_t green = 0U;

        if ( pad.top )
        {
            green = MINIMAL_LEVEL;
        }
        (void)kode_led_set_rgb(0U, green, 0U);
        kode_delay(MINIMAL_POLL_MS);
    }
}
