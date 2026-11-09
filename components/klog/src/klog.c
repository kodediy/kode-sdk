/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file klog.c
 * @brief klog's formatter, per-tag filter and ESP-IDF log capture.
 */
#include "kode_log.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define KLOG_LINE_MAX      256  /**< One formatted line, terminator included. */
#define KLOG_TAG_TABLE_SZ  16   /**< Per-tag overrides.                        */
#define KLOG_TAG_LEN       16   /**< A tag in the table, terminator included.  */
#define KLOG_STAMP_LEN     28   /**< A timestamp; ISO 8601 needs 25.           */
#define KLOG_ESP_TAG_LEN   32   /**< A tag parsed out of an ESP-IDF line.      */

/* The starting global level; INFO in a host build with no sdkconfig. */
#ifdef CONFIG_KLOG_DEFAULT_LEVEL
#define KLOG_BUILD_LEVEL  ((kode_log_lvl_t)CONFIG_KLOG_DEFAULT_LEVEL)
#else
#define KLOG_BUILD_LEVEL  KODE_LOG_INFO
#endif

/** @brief One per-tag minimum-level override. */
typedef struct {
    char           tag[KLOG_TAG_LEN]; /**< Tag it applies to ("" = free). */
    kode_log_lvl_t min;      /**< Lines below this level are dropped.       */
} klog_tag_lvl_t;

static klog_tag_lvl_t  s_tag_lvls[KLOG_TAG_TABLE_SZ];  /**< Per-tag overrides.   */
static kode_log_lvl_t  s_default_lvl = KLOG_BUILD_LEVEL; /**< Level for the rest. */
static bool            s_color       = false;          /**< ANSI colour on/off.  */
static kode_log_sink_t s_sink        = NULL;           /**< Sink; NULL = rescue. */

/** @brief The character each level prints as, indexed by kode_log_lvl_t. */
static const char s_lvl_char[] = { 'D', 'I', 'N', 'W', 'E' };

/* ANSI colour per level; off by default (kode_log_set_color()). Each escape
 * is its own literal: MISRA 4.1 wants an octal escape terminated. */
#define KLOG_C_RESET "\033" "[0m"
static const char *const s_lvl_color[] = {
    "\033" "[35m",   /* DEBUG  - magenta */
    "\033" "[32m",   /* INFO   - green   */
    "\033" "[36m",   /* NOTICE - cyan    */
    "\033" "[33m",   /* WARN   - yellow  */
    "\033" "[31m",   /* ERROR  - red     */
};

/* The holds (kode_log_hold()). A slot is free while its tag is NULL. The
 * level is written before the tag and the tag is a pointer to a string that
 * outlives the hold, so a task reading a slot while the holder fills or
 * empties it sees either no hold or a whole one. */
static const char *volatile s_hold_tag[KODE_LOG_HOLD_MAX]; /**< Held tags.   */
static volatile kode_log_lvl_t s_hold_min[KODE_LOG_HOLD_MAX]; /**< Their floors. */

/**
 * @brief The floor a hold puts under @p tag, or @p lvl when none is higher.
 *
 * @param tag The line's tag.
 * @param lvl The level the table or the global filter gave the tag.
 * @return The stricter of @p lvl and the tag's hold.
 */
static kode_log_lvl_t klog_apply_hold(const char *tag, kode_log_lvl_t lvl)
{
    kode_log_lvl_t min = lvl;
    for ( uint32_t i = 0U; KODE_LOG_HOLD_MAX > i; ++i )
    {
        const char *const held = s_hold_tag[i];
        if ( (NULL != held) && (strcmp(held, tag) == 0)
            && (s_hold_min[i] > min) )
        {
            min = s_hold_min[i];
        }
    }
    return min;
}

static kode_log_lvl_t klog_min_for_tag(const char *tag)
{
    for ( int i = 0; KLOG_TAG_TABLE_SZ > i; ++i )
    {
        if ( ('\0' != s_tag_lvls[i].tag[0])
            && (strncmp(s_tag_lvls[i].tag, tag, sizeof(s_tag_lvls[i].tag)) == 0) )
        {
            return klog_apply_hold(tag, s_tag_lvls[i].min);
        }
    }
    return klog_apply_hold(tag, s_default_lvl);
}

/**
 * @brief The ESP-IDF level matching a klog filter, so both move together.
 *
 * @param min klog minimum level.
 * @return The ESP-IDF threshold.
 */
static esp_log_level_t klog_espidf_level(kode_log_lvl_t min)
{
    switch ( min )
    {
        case KODE_LOG_DEBUG: return ESP_LOG_DEBUG;
        case KODE_LOG_INFO:  return ESP_LOG_INFO;
        /* ESP-IDF has no NOTICE: its nearest threshold is WARN. */
        case KODE_LOG_NOTICE: return ESP_LOG_WARN;
        case KODE_LOG_WARN:  return ESP_LOG_WARN;
        case KODE_LOG_ERROR: return ESP_LOG_ERROR;
        case KODE_LOG_OFF:   return ESP_LOG_NONE;
        default:             return ESP_LOG_NONE;
    }
}

void kode_log_emergency(const char *line)
{
    /* esp_rom_printf: no newlib, no locks, no allocation. */
    esp_rom_printf("%s", line);
}

/**
 * @brief Give a line to the sink, or write it directly when there is no sink,
 *        we are in an ISR or the scheduler is not running.
 */
static void klog_out(const char *line, size_t len, uint8_t src)
{
    if ( (NULL == s_sink)
        || (xPortCanYield() == pdFALSE)
        || (xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) )
    {
        kode_log_emergency(line);
        return;
    }
    s_sink(line, len, src);
}

/**
 * @brief Bound an snprintf result; a cut line still ends in '\n'.
 *
 * @param line The formatted buffer, at least @p cap bytes.
 * @param cap  The buffer capacity.
 * @param n    The raw snprintf return value.
 * @return The line's length, newline included.
 */
static size_t klog_close_line(char *line, size_t cap, int n)
{
    size_t len;
    if ( 0 > n )
    {
        len = 0U;
    }
    else if ( (size_t)n >= cap )
    {
        line[cap - 2U] = '\n';
        line[cap - 1U] = '\0';
        len = cap - 1U;
    }
    else
    {
        len = (size_t)n;
    }
    return len;
}

/**
 * @brief Write the timestamp: UTC ISO 8601 once the clock is set, else
 *        milliseconds since boot. The modulos only bound the fields for the
 *        compiler's buffer check.
 *
 * @param[out] buf Buffer for the stamp.
 * @param[in]  n   Its size.
 */
static void klog_stamp(char *buf, size_t n)
{
    struct timeval tv;
    struct tm      tm;

    if ( (gettimeofday(&tv, NULL) == 0)
        && ((time_t)KODE_CLOCK_KNOWN_AFTER_S < tv.tv_sec)
        && (gmtime_r(&tv.tv_sec, &tm) != NULL) )
    {
        unsigned const year = (unsigned)(tm.tm_year + 1900) % 10000U;
        unsigned const mon  = (unsigned)(tm.tm_mon + 1)     % 100U;
        unsigned const day  = (unsigned)tm.tm_mday          % 100U;
        unsigned const hour = (unsigned)tm.tm_hour          % 100U;
        unsigned const min  = (unsigned)tm.tm_min           % 100U;
        unsigned const sec  = (unsigned)tm.tm_sec           % 100U;
        unsigned const ms   = (unsigned)(tv.tv_usec / 1000) % 1000U;

        (void)snprintf(buf, n, "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
                       year, mon, day, hour, min, sec, ms);
        return;
    }

    /* Wraps after about 49 days without the clock set. */
    (void)snprintf(buf, n, "%lu",
                   (unsigned long)((uint32_t)(esp_timer_get_time() / 1000LL)));
}

static void klog_emit(kode_log_lvl_t lvl, const char *tag, const char *msg)
{
    char stamp[KLOG_STAMP_LEN];
    char line[KLOG_LINE_MAX];
    const char *col = "";
    const char *rst = "";

    if ( s_color )
    {
        col = s_lvl_color[lvl];
        rst = KLOG_C_RESET;
    }

    klog_stamp(stamp, sizeof(stamp));

    int const n = snprintf(line, sizeof(line), "%s!%c %s %s %s%s\n",
                           col, s_lvl_char[lvl], stamp, tag, msg, rst);
    klog_out(line, klog_close_line(line, sizeof(line), n), KODE_LOG_SRC_NONE);
}

void kode_log_va(kode_log_lvl_t lvl, const char *tag, const char *fmt, va_list ap)
{
    /* Past ERROR is refused: klog_emit() indexes two five-entry tables. */
    if ( (NULL == tag) || (NULL == fmt) || (KODE_LOG_ERROR < lvl)
        || (klog_min_for_tag(tag) > lvl) )
    {
        return;
    }
    char msg[KLOG_LINE_MAX];
    (void)vsnprintf(msg, sizeof(msg), fmt, ap);
    klog_emit(lvl, tag, msg);
}

void kode_log(kode_log_lvl_t lvl, const char *tag, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    kode_log_va(lvl, tag, fmt, ap);
    va_end(ap);
}

void kode_log_always(kode_log_lvl_t lvl, const char *tag, const char *fmt, ...)
{
    if ( (NULL == tag) || (NULL == fmt) || (KODE_LOG_ERROR < lvl)
        || (KODE_LOG_OFF == s_default_lvl) )
    {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    char msg[KLOG_LINE_MAX];
    (void)vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    klog_emit(lvl, tag, msg);
}

void kode_log_reply_src(const char *line, uint8_t src)
{
    /* Verbatim plus a newline, through the same sink as the log lines. */
    char buf[KLOG_LINE_MAX];
    int const n = snprintf(buf, sizeof(buf), "%s\n", line);
    klog_out(buf, klog_close_line(buf, sizeof(buf), n), src);
}

void kode_log_reply(const char *line)
{
    kode_log_reply_src(line, KODE_LOG_SRC_NONE);
}

void kode_log_set_level(const char *tag, kode_log_lvl_t min)
{
    if ( NULL == tag )
    {
        s_default_lvl = min;
        esp_log_level_set("*", klog_espidf_level(min));
        return;
    }
    /* A tag that does not fit whole is refused: cut, it would never match. */
    if ( strnlen(tag, sizeof(s_tag_lvls[0].tag)) >= sizeof(s_tag_lvls[0].tag) )
    {
        kode_log(KODE_LOG_WARN, "klog",
                 "tag '%.*s...' is too long for a per-tag level; ignored",
                 (int)(sizeof(s_tag_lvls[0].tag) - 1U), tag);
        return;
    }
    int free_slot = -1;
    for ( int i = 0; KLOG_TAG_TABLE_SZ > i; ++i )
    {
        if ( '\0' == s_tag_lvls[i].tag[0] )
        {
            if ( 0 > free_slot )
            {
                free_slot = i;
            }
        }
        else if ( strncmp(s_tag_lvls[i].tag, tag, sizeof(s_tag_lvls[i].tag)) == 0 )
        {
            s_tag_lvls[i].min = min;
            esp_log_level_set(tag, klog_espidf_level(min));
            return;
        }
    }
    if ( 0 <= free_slot )
    {
        (void)strncpy(s_tag_lvls[free_slot].tag, tag,
                      sizeof(s_tag_lvls[free_slot].tag) - 1U);
        s_tag_lvls[free_slot].min = min;
        esp_log_level_set(tag, klog_espidf_level(min));
    }
}

/* cppcheck-suppress misra-c2012-8.7 ; kode_log.h's interface: a driver holds a tag whose lines can carry a secret */
bool kode_log_hold(const char *tag, kode_log_lvl_t min)
{
    if ( NULL == tag )
    {
        return false;
    }
    uint32_t slot = KODE_LOG_HOLD_MAX;
    for ( uint32_t i = 0U; KODE_LOG_HOLD_MAX > i; ++i )
    {
        const char *const held = s_hold_tag[i];
        if ( (NULL != held) && (strcmp(held, tag) == 0) )
        {
            slot = i;
            break;
        }
        if ( (NULL == held) && (KODE_LOG_HOLD_MAX == slot) )
        {
            slot = i;
        }
    }
    if ( KODE_LOG_HOLD_MAX == slot )
    {
        return false;
    }
    s_hold_min[slot] = min;
    s_hold_tag[slot] = tag;
    return true;
}

/* cppcheck-suppress misra-c2012-8.7 ; kode_log.h's interface: the holder ends its hold */
void kode_log_release(const char *tag)
{
    if ( NULL == tag )
    {
        return;
    }
    for ( uint32_t i = 0U; KODE_LOG_HOLD_MAX > i; ++i )
    {
        const char *const held = s_hold_tag[i];
        if ( (NULL != held) && (strcmp(held, tag) == 0) )
        {
            s_hold_tag[i] = NULL;
        }
    }
}

kode_log_lvl_t kode_log_get_level(void)
{
    return s_default_lvl;
}

kode_log_lvl_t kode_log_build_level(void)
{
    return KLOG_BUILD_LEVEL;
}

void kode_log_set_color(bool enable)
{
    s_color = enable;
}

void kode_log_set_sink(kode_log_sink_t sink)
{
    s_sink = sink;
}

/* ESP-IDF's vprintf hook: parse "I (1234) tag: msg" back out and re-emit it
 * through klog; a line it cannot parse goes out at DEBUG as tag "esp". A
 * vendor's INFO is DEBUG here: kodeOS says at INFO what came up, and the
 * drivers under it tell the same story in their own words. */
static int klog_espidf_vprintf(const char *fmt, va_list ap)
{
    char line[KLOG_LINE_MAX];
    int const n = vsnprintf(line, sizeof(line), fmt, ap);

    kode_log_lvl_t lvl = KODE_LOG_DEBUG;
    char const    *p   = line;

    switch ( line[0] )
    {
        case 'E': lvl = KODE_LOG_ERROR; break;
        case 'W': lvl = KODE_LOG_WARN;  break;
        case 'I': lvl = KODE_LOG_DEBUG; break;
        case 'D': /* fall through */
        case 'V': lvl = KODE_LOG_DEBUG; break;
        default:  p = NULL;             break;
    }

    char tag[KLOG_ESP_TAG_LEN] = "esp";
    char const *msg = line;

    if ( (NULL != p) && (' ' == line[1]) && ('(' == line[2]) )
    {
        char const * const close = strchr(&line[3], ')');
        if ( (NULL != close) && (' ' == close[1]) )
        {
            char const * const tag_start = close + 2;
            char const * const colon     = strstr(tag_start, ": ");
            if ( NULL != colon )
            {
                size_t tag_len = (size_t)(colon - tag_start);
                if ( sizeof(tag) <= tag_len )
                {
                    tag_len = sizeof(tag) - 1U;
                }
                (void)memcpy(tag, tag_start, tag_len);
                tag[tag_len] = '\0';
                msg = colon + 2;
            }
        }
    }

    char clean[KLOG_LINE_MAX];
    (void)strncpy(clean, msg, sizeof(clean) - 1U);
    clean[sizeof(clean) - 1U] = '\0';
    size_t len = strlen(clean);
    while ( (0U < len) && (('\n' == clean[len - 1U]) || ('\r' == clean[len - 1U])) )
    {
        clean[--len] = '\0';
    }

    if ( klog_min_for_tag(tag) <= lvl )
    {
        klog_emit(lvl, tag, clean);
    }
    return n;
}

void kode_log_init(void)
{
    (void)esp_log_set_vprintf(&klog_espidf_vprintf);
    KLOG_D("klog", "kodeOS logging up (ESP-IDF stream captured)");
}
