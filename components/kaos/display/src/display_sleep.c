/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file display_sleep.c
 * @brief The glass's rest and the last sleep request's end, as two words.
 */
#include "display_sleep.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "kaos.h"

/** @brief Where the count of answers starts in the word of the last answer. */
#define SLEEP_LAST_ANSWERS_SHIFT 16U

/** @brief Where the reason starts in the word of the last answer. */
#define SLEEP_LAST_WHY_SHIFT     8U

/** @brief The bits of one byte. */
#define SLEEP_LAST_BYTE_MASK     0xFFU

/** @brief The bits of the count of answers, once shifted down. */
#define SLEEP_LAST_ANSWERS_MASK  0xFFFFU

/** @brief Whether the glass is at rest. */
static _Atomic bool s_glass_asleep;

/** @brief The last answer: its count, its reason and its status, high to low. */
static _Atomic uint32_t s_sleep_answer;

void display_sleep_glass_set(bool asleep)
{
    atomic_store_explicit(&s_glass_asleep, asleep, memory_order_release);
}

void display_sleep_answer_set(kode_status_t rc, display_sleep_why_t why)
{
    /* Only AO_Display's thread writes the word, so load + store is whole. */
    uint32_t const last    = atomic_load_explicit(&s_sleep_answer, memory_order_relaxed);
    uint32_t const answers = ((last >> SLEEP_LAST_ANSWERS_SHIFT) + 1U) & SLEEP_LAST_ANSWERS_MASK;
    uint32_t const word    = (answers << SLEEP_LAST_ANSWERS_SHIFT)
                             | (((uint32_t)why & SLEEP_LAST_BYTE_MASK) << SLEEP_LAST_WHY_SHIFT)
                             | ((uint32_t)rc & SLEEP_LAST_BYTE_MASK);

    atomic_store_explicit(&s_sleep_answer, word, memory_order_release);
}

bool display_asleep(void)
{
    return atomic_load_explicit(&s_glass_asleep, memory_order_acquire);
}

void display_sleep_last(display_sleep_last_t *out)
{
    if ( NULL != out )
    {
        uint32_t const last = atomic_load_explicit(&s_sleep_answer, memory_order_acquire);

        out->answers = (uint16_t)(last >> SLEEP_LAST_ANSWERS_SHIFT);
        out->why     = (uint8_t)((last >> SLEEP_LAST_WHY_SHIFT) & SLEEP_LAST_BYTE_MASK);
        out->rc      = (uint8_t)(last & SLEEP_LAST_BYTE_MASK);
    }
}

const char *display_sleep_why_text(uint8_t why)
{
    /* Each reason in words, by display_sleep_why_t. */
    static const char * const s_why_text[DISPLAY_SLEEP_WHY_COUNT] = {
        [DISPLAY_SLEEP_WHY_NONE]     = "",
        [DISPLAY_SLEEP_WHY_NO_PANEL] = "the panel is not up",
        [DISPLAY_SLEEP_WHY_LENT]     = "a screenshot or the mirror is reading it",
        [DISPLAY_SLEEP_WHY_TEST]     = "the display test is on it",
        [DISPLAY_SLEEP_WHY_RENDERER] = "the renderer did not stop drawing",
        [DISPLAY_SLEEP_WHY_PANEL]    = "the panel or its link refused",
        [DISPLAY_SLEEP_WHY_BOARD]    = "the board's rest has it",
        [DISPLAY_SLEEP_WHY_UNDERWAY] = "an earlier request is still under way",
        [DISPLAY_SLEEP_WHY_WOKEN]    = "it was woken before it slept",
        [DISPLAY_SLEEP_WHY_EVENTS]   = "no event was free to stop the renderer",
    };

    const char *text = "";

    if ( (uint8_t)DISPLAY_SLEEP_WHY_COUNT > why )
    {
        text = s_why_text[why];
    }
    return text;
}
