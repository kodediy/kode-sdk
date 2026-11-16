/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file lowpower_part.c
 * @brief The rules of an app's rest: which parts may rest together, and in what order.
 */
#include "lowpower_part.h"

#include <stddef.h>

#include "kqp.h"

/** @brief One part: its bit, its name and the owner that rests it. */
typedef struct {
    uint8_t     bit;    /**< The KODE_LP_PART_* bit.                 */
    const char *name;   /**< What the log and a refusal call it.     */
    const char *owner;  /**< The name its owner joined the rounds under. */
} Part;

/** @brief The parts, in the order they are put away. */
static const Part DOWN[] = {
    { KODE_LP_PART_SOUND,  "sound",  "audio"   },
    { KODE_LP_PART_CARD,   "card",   "storage" },
    { KODE_LP_PART_TOUCH,  "touch",  "touch"   },
    { KODE_LP_PART_SCREEN, "screen", "display" },
    { KODE_LP_PART_PANEL,  "panel",  "display" },
    { KODE_LP_PART_RADIO,  "radio",  "radio"   },
    { KODE_LP_PART_CPU,    "cpu",    "ioexp"   },
};

/** @brief The parts that are put away once and come back at their next use. */
#define ONCE       ((uint8_t)(KODE_LP_PART_SOUND | KODE_LP_PART_CARD))

/* One ask a part, and the CPU part asks a second owner for its wake time. */
_Static_assert(KODE_LOWPOWER_PART_ASKS == (Q_DIM(DOWN) + 1U),
               "the longest rest's wait covers every owner it asks");

/** @brief The parts that leave the glass at rest. */
#define GLASS      ((uint8_t)(KODE_LP_PART_SCREEN | KODE_LP_PART_PANEL))

/** @brief The parts that leave the touch controller unable to interrupt. */
#define TOUCH_OFF  ((uint8_t)(KODE_LP_PART_TOUCH | KODE_LP_PART_PANEL))

/**
 * @brief The entry of one part.
 * @param part One KODE_LP_PART_* bit.
 * @return Its entry; NULL for a value that is no single part.
 */
static const Part *part_find(uint8_t part)
{
    const Part *found = NULL;

    for ( uint8_t i = 0U; Q_DIM(DOWN) > i; i++ )
    {
        if ( DOWN[i].bit == part )
        {
            found = &DOWN[i];
        }
    }
    return found;
}

const char *lowpower_part_check(uint8_t *parts, const lowpower_part_ask_t *ask)
{
    const char *why = NULL;
    uint8_t     want;
    uint8_t     held;

    if ( (NULL == parts) || (NULL == ask) )
    {
        return "request: no parts";
    }
    want = *parts;
    if ( (want & KODE_LP_PART_PANEL) != 0U )
    {
        want |= (uint8_t)(KODE_LP_PART_SCREEN | KODE_LP_PART_TOUCH);
    }
    /* What the rest would hold once this request is done. */
    held = want | ask->at_rest;

    if ( (0U == want) || ((want & (uint8_t)~KODE_LP_PART_ALL) != 0U) )
    {
        why = "request: no such part";
    }
    else if ( ((want & KODE_LP_PART_RADIO) != 0U) && ((want & KODE_LP_PART_SOUND) == 0U) )
    {
        /* The amplifier's enable is a line of the C5, and a sound put away
         * before may have come back: its owner is asked in this request. */
        why = "radio: needs sound at rest";
    }
    else if ( (want & KODE_LP_PART_CPU) == 0U )
    {
        /* The wake sources are the CPU part's: nothing more to judge. */
    }
    else if ( ask->cable )
    {
        why = "cable: external power";
    }
    else if ( ask->glass && ((held & GLASS) == 0U) )
    {
        /* The panel's link holds the lock that keeps the P4 out of light sleep. */
        why = "cpu: needs the screen at rest";
    }
    else if ( ask->wake_touch && ((held & TOUCH_OFF) != 0U) )
    {
        why = "touch: at rest cannot wake";
    }
    else if ( ask->wake_pin && !ask->pin_known )
    {
        why = "pin: no such position";
    }
    else if ( ask->wake_pin && !ask->pin_free )
    {
        /* An output, a PWM, an analog input or a loan to a driver: its owner
         * has it, and a wake line would take it from under it. */
        why = "pin: in use";
    }
    else
    {
        /* The request stands. */
    }
    if ( NULL == why )
    {
        *parts = want;
    }
    return why;
}

uint8_t lowpower_part_next_down(uint8_t want, uint8_t done)
{
    uint8_t next = 0U;

    for ( uint8_t i = 0U; (Q_DIM(DOWN) > i) && (0U == next); i++ )
    {
        if ( ((want & DOWN[i].bit) != 0U) && ((done & DOWN[i].bit) == 0U) )
        {
            next = DOWN[i].bit;
        }
    }
    return next;
}

uint8_t lowpower_part_next_up(uint8_t at_rest)
{
    /* The parts, in the order they are brought back. */
    static const uint8_t UP[] = {
        KODE_LP_PART_CPU, KODE_LP_PART_RADIO, KODE_LP_PART_PANEL, KODE_LP_PART_SCREEN,
        KODE_LP_PART_TOUCH,
    };
    uint8_t next = 0U;

    for ( uint8_t i = 0U; (Q_DIM(UP) > i) && (0U == next); i++ )
    {
        if ( (at_rest & UP[i]) != 0U )
        {
            next = UP[i];
        }
    }
    return next;
}

bool lowpower_part_stays(uint8_t part)
{
    return (part & ONCE) == 0U;
}

const char *lowpower_part_name(uint8_t part)
{
    const Part * const p    = part_find(part);
    const char        *name = "part";

    if ( NULL != p )
    {
        name = p->name;
    }
    return name;
}

const char *lowpower_part_owner(uint8_t part)
{
    const Part * const p     = part_find(part);
    const char        *owner = "";

    if ( NULL != p )
    {
        owner = p->owner;
    }
    return owner;
}
