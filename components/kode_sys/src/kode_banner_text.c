/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_banner_text.c
 * @brief The banner's words: the reset reason table and the reset line (pure text).
 */
#include "kode_banner.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "kdrv_slot.h"

/** @brief One reset reason and what the banner says for it. */
typedef struct {
    int32_t     reason; /**< A KDRV_RESET_ value. */
    char const *words;  /**< What the banner says. */
} reset_word_t;

char const *kode_reset_words(int32_t reason)
{
    /* Every reason ESP-IDF's esp_reset_reason_t knows; the first entry answers an unlisted number. */
    static reset_word_t const words[] = {
        { (int32_t)KDRV_RESET_UNKNOWN,    "unknown" },
        { (int32_t)KDRV_RESET_POWERON,    "power-on or BOT+LEFT" },
        { (int32_t)KDRV_RESET_EXTERNAL,   "external reset" },
        { (int32_t)KDRV_RESET_SOFTWARE,   "software restart" },
        { (int32_t)KDRV_RESET_PANIC,      "panic" },
        { (int32_t)KDRV_RESET_INT_WDT,    "interrupt watchdog" },
        { (int32_t)KDRV_RESET_TASK_WDT,   "task watchdog" },
        { (int32_t)KDRV_RESET_WDT,        "other watchdog" },
        { (int32_t)KDRV_RESET_DEEPSLEEP,  "deep-sleep wake" },
        { (int32_t)KDRV_RESET_BROWNOUT,   "brownout" },
        { (int32_t)KDRV_RESET_SDIO,       "SDIO reset" },
        { (int32_t)KDRV_RESET_USB,        "USB port opened" },
        { (int32_t)KDRV_RESET_JTAG,       "JTAG reset" },
        { (int32_t)KDRV_RESET_EFUSE,      "eFuse error" },
        { (int32_t)KDRV_RESET_PWR_GLITCH, "power glitch" },
        { (int32_t)KDRV_RESET_CPU_LOCKUP, "CPU lockup" },
    };
    size_t const count = sizeof(words) / sizeof(words[0]);

    for ( size_t i = 0U; i < count; ++i )
    {
        if ( words[i].reason == reason )
        {
            return words[i].words;
        }
    }
    return words[0].words;
}

bool kode_banner_dump_explains(int32_t reason)
{
    return ((int32_t)KDRV_RESET_PANIC == reason)
        || ((int32_t)KDRV_RESET_INT_WDT == reason)
        || ((int32_t)KDRV_RESET_TASK_WDT == reason);
}

size_t kode_banner_reset_line(char *out, size_t len, int32_t reason,
                              char const *dump_reason, char const *app_name)
{
    if ( (NULL == out) || (0U == len) )
    {
        return 0U;
    }

    char const *why = kode_reset_words(reason);
    if ( (NULL == app_name) && kode_banner_dump_explains(reason)
        && (NULL != dump_reason) && ('\0' != dump_reason[0]) )
    {
        why = dump_reason;
    }

    int32_t n;
    if ( NULL != app_name )
    {
        n = snprintf(out, len, "reset back from app %s: %s", app_name, why);
    }
    else
    {
        n = snprintf(out, len, "reset %s", why);
    }
    if ( 0 > n )
    {
        out[0] = '\0';
        return 0U;
    }
    if ( (size_t)n >= len )
    {
        return len - 1U;
    }
    return (size_t)n;
}
