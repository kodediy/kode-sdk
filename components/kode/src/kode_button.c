/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_button.c
 * @brief The standard layer's buttons, read by name.
 */
#include "kode.h"

#include <stdatomic.h>

#include "kdrv_buttons.h"
#include "kode_button.h"
#include "kode_entry.h"
#include "kode_sys.h"

/** @brief What a name lookup answers for a name that is no button: an id past the last. */
#define KODE_BUTTON_NONE ((uint8_t)KDRV_BTN_COUNT)

/** @brief The suggestion offered for the name people expect: "bot". */
static const char s_short_bottom[] = "bot";

/** @brief The buttons' names, indexed by kdrv_button_t. */
static const char *const s_names[KDRV_BTN_COUNT] = {
    "up", "down", "left", "right", "top", "bottom"
};

/** @brief The press count each button had when it was last asked for. */
static atomic_uint_least32_t s_seen[KDRV_BTN_COUNT];

/**
 * @brief The id of the button called @p name, or KODE_BUTTON_NONE with the
 *        task's error set.
 */
static uint8_t button_find(const char *name)
{
    uint8_t found = KODE_BUTTON_NONE;

    if ( NULL == name )
    {
        kode_err_set("button name is NULL");
    }
    else
    {
        for ( uint8_t i = 0U; ((uint8_t)KDRV_BTN_COUNT > i) && (KODE_BUTTON_NONE == found); i++ )
        {
            if ( kode_name_is(name, s_names[i]) )
            {
                found = i;
            }
        }
        if ( KODE_BUTTON_NONE != found )
        {
            /* a button: nothing to say */
        }
        else if ( kode_name_is(name, s_short_bottom) )
        {
            kode_err_set("unknown button '%s' (did you mean 'bottom'?)", name);
        }
        else
        {
            kode_err_set("unknown button '%s'", name);
        }
    }
    return found;
}

const char *kode_button_name(uint8_t id)
{
    const char *name = NULL;

    if ( (uint8_t)KDRV_BTN_COUNT > id )
    {
        name = s_names[id];
    }
    return name;
}

uint32_t kode_button_take(uint8_t id, kode_button_reader_t read)
{
    uint32_t delta = 0U;

    if ( (uint8_t)KDRV_BTN_COUNT > id )
    {
        uint_least32_t seen = atomic_load(&s_seen[id]);
        uint_least32_t now  = (uint_least32_t)read(id);

        while ( !atomic_compare_exchange_weak(&s_seen[id], &seen, now) )
        {
            now = (uint_least32_t)read(id);
        }
        delta = (uint32_t)now - (uint32_t)seen;
    }
    return delta;
}

bool kode_button_is_pressed(const char *name)
{
    bool down = false;

    if ( kode_enter("kode_button_is_pressed") )
    {
        const uint8_t id = button_find(name);

        if ( KODE_BUTTON_NONE != id )
        {
            down = ((kode_sys_button_read_mask() >> id) & 1U) != 0U;
        }
    }
    return down;
}

bool kode_button_was_pressed(const char *name)
{
    bool news = false;

    if ( kode_enter("kode_button_was_pressed") )
    {
        const uint8_t id = button_find(name);

        if ( KODE_BUTTON_NONE != id )
        {
            news = kode_button_take(id, &kode_sys_button_read_presses) != 0U;
        }
    }
    return news;
}

uint32_t kode_button_get_presses(const char *name)
{
    uint32_t presses = 0U;

    if ( kode_enter("kode_button_get_presses") )
    {
        const uint8_t id = button_find(name);

        if ( KODE_BUTTON_NONE != id )
        {
            presses = kode_button_take(id, &kode_sys_button_read_presses);
        }
    }
    return presses;
}

kode_buttons_t kode_button_read_all(void)
{
    kode_buttons_t pad = { false, false, false, false, false, false };

    if ( kode_enter("kode_button_read_all") )
    {
        const uint32_t mask = kode_sys_button_read_mask();

        pad.up     = ((mask >> (uint32_t)KDRV_BTN_UP) & 1U) != 0U;
        pad.down   = ((mask >> (uint32_t)KDRV_BTN_DOWN) & 1U) != 0U;
        pad.left   = ((mask >> (uint32_t)KDRV_BTN_LEFT) & 1U) != 0U;
        pad.right  = ((mask >> (uint32_t)KDRV_BTN_RIGHT) & 1U) != 0U;
        pad.top    = ((mask >> (uint32_t)KDRV_BTN_TOP) & 1U) != 0U;
        pad.bottom = ((mask >> (uint32_t)KDRV_BTN_BOT) & 1U) != 0U;
    }
    return pad;
}
