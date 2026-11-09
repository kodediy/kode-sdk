/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_log.h
 * @brief klog: the only log front end of the SDK; AO_Log owns the wire.
 */
#ifndef KODE_LOG_H_
#define KODE_LOG_H_

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 2026-01-01T00:00:00Z: a wall clock at or before this was never set.
 *        Here and not in khal, because khal includes klog.
 */
#define KODE_CLOCK_KNOWN_AFTER_S  1767225600L

/**
 * @brief The five levels plus OFF, the filter that drops them all. A line
 *        passes when its level is `>=` the filter.
 */
typedef enum {
    KODE_LOG_DEBUG = 0,   /**< Bench detail.                                */
    KODE_LOG_INFO,        /**< What happened, for someone reading later.    */
    KODE_LOG_NOTICE,      /**< Outcomes, answers and state changes.         */
    KODE_LOG_WARN,        /**< Wrong but survivable.                        */
    KODE_LOG_ERROR,       /**< The call failed and somebody has to know.    */
    KODE_LOG_OFF,         /**< Filter only: drops every line.               */
} kode_log_lvl_t;

/** @brief The console's word for each level, indexed by kode_log_lvl_t. */
#define KODE_LOG_LEVEL_WORDS  { "d", "i", "n", "w", "e", "off" }

/**
 * @brief Start klog and route ESP-IDF's logs through it. Once, first in
 *        kode_init(); needs no scheduler.
 */
void kode_log_init(void);

/**
 * @brief Format one line and hand it to the wire; use the KLOG_x macros.
 *
 * Dropped when @p lvl is under the filter for @p tag. Cut to 256 bytes,
 * always newline-terminated. Any task; not from an ISR with the cache off.
 *
 * @param lvl Level; KODE_LOG_OFF or out of range drops the line.
 * @param tag Component tag; NULL drops the line.
 * @param fmt printf format; NULL drops the line.
 */
void kode_log(kode_log_lvl_t lvl, const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/**
 * @brief Format one line that the level filters do not hold back.
 *
 * For the lines that must be seen whatever the console is set to: the boot
 * banner (NOTICE) and the supervisor's slow-step warning (WARN). It is on the
 * wire at every level, the global filter and the tag's own included, except
 * when the global level is KODE_LOG_OFF, which still means off. Otherwise
 * exactly kode_log().
 *
 * @param lvl Level the line prints as; past KODE_LOG_ERROR drops the line.
 * @param tag Component tag; NULL drops the line.
 * @param fmt printf format; NULL drops the line.
 */
void kode_log_always(kode_log_lvl_t lvl, const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/**
 * @brief kode_log() with a va_list.
 *
 * @param lvl See kode_log().
 * @param tag See kode_log().
 * @param fmt See kode_log().
 * @param ap  The arguments; the caller does va_start and va_end.
 */
void kode_log_va(kode_log_lvl_t lvl, const char *tag, const char *fmt,
                 va_list ap);

/**
 * @brief Set the filter for one tag (up to 16 tags of 15 characters) or the
 *        global one; ESP-IDF's tag filter follows. The `log_level` verb.
 *
 * @param tag The tag, or NULL for the global default.
 * @param min Lines below this level are dropped.
 */
void kode_log_set_level(const char *tag, kode_log_lvl_t min);

#define KODE_LOG_HOLD_MAX  4U  /**< Tags held at once (kode_log_hold()). */

/**
 * @brief Hold one tag at @p min or quieter until kode_log_release(), whatever
 *        the global filter and the tag's own level say meanwhile.
 *
 * For a tag whose quiet lines can carry a secret: ESP-IDF's HTTP client
 * writes a request's headers, `Authorization` included, at DEBUG. A hold is
 * the floor, not a level: neither kode_log_set_level(NULL, …) (`config
 * key="log"`) nor kode_log_set_level(tag, …) (`log_level`) lowers it, and a
 * tag set quieter than @p min stays quieter. ESP-IDF's lines are judged by
 * the tag they name, so the hold drops them too.
 *
 * @param tag A string that outlives the hold (a literal); at most
 *            KODE_LOG_HOLD_MAX tags at a time, one holder each.
 * @param min Lines of @p tag below this level are dropped.
 * @return true when the hold stands; false when @p tag is NULL or every slot
 *         is taken, and nothing is held.
 */
bool kode_log_hold(const char *tag, kode_log_lvl_t min);

/**
 * @brief End the hold kode_log_hold() put on @p tag. Safe when none stands.
 *
 * @param tag The tag that was held.
 */
void kode_log_release(const char *tag);

/**
 * @brief The global filter, as `info` reports it.
 *
 * @return The current global level.
 */
kode_log_lvl_t kode_log_get_level(void);

/**
 * @brief The global filter before anything is stored (CONFIG_KLOG_DEFAULT_LEVEL).
 *
 * @return The build's starting level.
 */
kode_log_lvl_t kode_log_build_level(void);

/**
 * @brief Colour each line by level (off by default). The `log_color` verb.
 *
 * @param enable true to colour, false for plain.
 */
void kode_log_set_color(bool enable);

/**
 * @brief The source of a line that answers no command: every `!` line, and a
 *        `>` line sent through kode_log_reply().
 *
 * klog does not interpret a source. The console tags its replies with the
 * port of the command they answer (KODE_CONSOLE_SRC_*), and whoever owns the
 * sink decides what to do with the tag.
 */
#define KODE_LOG_SRC_NONE  0xFFU

/**
 * @brief Send one console response line (`>ok`, `>|...`) verbatim, unfiltered.
 *
 * @param line The line without its newline, under 256 bytes.
 */
void kode_log_reply(const char *line);

/**
 * @brief Put one response line on the wire, tagged with the port of the
 *        command it answers.
 *
 * Exactly kode_log_reply(), plus the tag: the sink receives @p src with the
 * line, and kode_log_reply() is this with KODE_LOG_SRC_NONE. The tag does not
 * change what is written to the cable or to BLE.
 *
 * @param line NUL-terminated response line without the trailing newline,
 *             shorter than the formatter's 256-byte line buffer.
 * @param src  KODE_CONSOLE_SRC_* of the command answered, or
 *             KODE_LOG_SRC_NONE.
 */
void kode_log_reply_src(const char *line, uint8_t src);

/**
 * @brief Where finished lines go: AO_Log's hook, which posts them on.
 *
 * @param line The finished line, newline included; valid during the call only.
 * @param len  Its length, newline included.
 * @param src  The tag kode_log_reply_src() was given; KODE_LOG_SRC_NONE for a
 *             log line and for kode_log_reply().
 */
typedef void (*kode_log_sink_t)(const char *line, size_t len, uint8_t src);

/**
 * @brief Register the sink, or NULL to fall back to the rescue path.
 *
 * @param sink The sink, or NULL.
 */
void kode_log_set_sink(kode_log_sink_t sink);

/**
 * @brief The rescue path: write directly, with no sink, lock or stdio.
 *        For before QF runs, ISRs and asserts only; otherwise use KLOG_x.
 *
 * @param line The line, newline included; written verbatim.
 */
void kode_log_emergency(const char *line);

/**
 * @brief The most verbose level compiled in (CONFIG_KLOG_MAXIMUM_LEVEL, or all
 *        without an sdkconfig). A file may define it first, like LOG_LOCAL_LEVEL.
 */
#ifndef KLOG_MAXIMUM_LEVEL
#  if defined(__has_include)
#    if __has_include("sdkconfig.h")
#      include "sdkconfig.h"
#    endif
#  endif
#  ifdef CONFIG_KLOG_MAXIMUM_LEVEL
#    define KLOG_MAXIMUM_LEVEL  CONFIG_KLOG_MAXIMUM_LEVEL
#  else
#    define KLOG_MAXIMUM_LEVEL  0
#  endif
#endif

/**
 * @brief A line under the ceiling: no format string in flash, but its
 *        arguments still run and are still type-checked.
 */
static inline __attribute__((always_inline, format(printf, 2, 3)))
void kode_log_nop(const char *tag, const char *fmt, ...)
{
    (void)tag;
    (void)fmt;
}

/* An expression, so KLOG_x works wherever a call to kode_log() would. */
#define KLOG_AT_(lvl, tag, ...) \
    (((int)(lvl) >= (int)(KLOG_MAXIMUM_LEVEL)) \
         ? kode_log((lvl), (tag), __VA_ARGS__) : kode_log_nop((tag), __VA_ARGS__))

/** @brief Log at KODE_LOG_DEBUG. */
#define KLOG_D(tag, ...) KLOG_AT_(KODE_LOG_DEBUG,  tag, __VA_ARGS__)
/** @brief Log at KODE_LOG_INFO. */
#define KLOG_I(tag, ...) KLOG_AT_(KODE_LOG_INFO,   tag, __VA_ARGS__)
/** @brief Log at KODE_LOG_NOTICE. */
#define KLOG_N(tag, ...) KLOG_AT_(KODE_LOG_NOTICE, tag, __VA_ARGS__)
/** @brief Log at KODE_LOG_WARN. */
#define KLOG_W(tag, ...) KLOG_AT_(KODE_LOG_WARN,   tag, __VA_ARGS__)
/** @brief Log at KODE_LOG_ERROR. */
#define KLOG_E(tag, ...) KLOG_AT_(KODE_LOG_ERROR,  tag, __VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* KODE_LOG_H_ */
