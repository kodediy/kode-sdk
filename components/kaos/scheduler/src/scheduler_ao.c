/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file scheduler_ao.c
 * @brief AO_Scheduler: a table of cron entries that run if the board is awake at their minute.
 */
#include "kaos.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "kdrv_clocks.h"
#include "khal_nvs.h"
#include "khal_system.h"
#include "khal_time.h"
#include "kode_log.h"
#include "kqp.h"
#include "sdkconfig.h"

#include "sched_cron.h"
#include "kaos_claim.h"
#include "kaos_start.h"
#include "kverbs.h"

#define TAG "kaos.sched"

#if CONFIG_KODEOS_NET && CONFIG_KODE_LOWPOWER
/* AO_Net, named weakly (KAOS_WEAK, kaos.h): the check time is handed to an
 * AO_Net the app linked, and AO_Scheduler must not link it itself. */
KAOS_WEAK void kaos_net_start(void);
KAOS_WEAK bool net_check_at(int64_t due_wall_s);
#endif

/** @brief Microseconds in a second, for the wall clock. */
#define SCHED_US_PER_S      1000000LL

/** @brief Microseconds in a second, unsigned, for dividing the wall clock's reading. */
#define SCHED_US_PER_S_U    1000000ULL

/** @brief Microseconds in a millisecond. */
#define SCHED_US_PER_MS     1000LL

/** @brief The first slot: the built-in `mqtt`. */
#define SCHED_SLOT_MQTT     0U

/** @brief A slot number that is none. */
#define SCHED_SLOT_NONE     0xFFU

/** @brief A minute that is never reached: the timer is armed for no minute. */
#define SCHED_MINUTE_NONE   INT64_MAX

/** @brief No minute has run yet. */
#define SCHED_MINUTE_UNSET  (-1LL)

/** @brief A time that matches no minute, or none yet. */
#define SCHED_NO_TIME       (-1LL)

/** @brief The reference an expression is tried from to see that it can ever match: the epoch. */
#define SCHED_PROBE_FROM_S  0LL

/** @brief Alignment of the table in PSRAM. */
#define SCHED_TABLE_ALIGN   8U

/** @brief The NVS namespace of the table. */
#define SCHED_STORE_NS      "kcron"

/** @brief The key of the `mqtt` record. */
#define SCHED_KEY_MQTT      "mqtt"

/** @brief The key of a console entry's record, from its slot. */
#define SCHED_KEY_FMT       "e%u"

/** @brief Bytes of a key: "e15" and its NUL, with room. */
#define SCHED_KEY_MAX       8U

/** @brief The layout of a record; another version reads as not stored. */
#define SCHED_STORE_VERSION 1U

/** @brief Bytes of a record's version. */
#define SCHED_STORE_VERSION_BYTES 1U

/** @brief Bytes of a record: the version, then the three texts. */
#define SCHED_STORE_BYTES   (SCHED_STORE_VERSION_BYTES + (KODE_SCHED_NAME_MAX + 1U) \
                             + (KODE_SCHED_WHEN_MAX + 1U) + KODE_SCHED_CMD_MAX)

/** @brief Slots a key of one letter and two digits can name. */
#define SCHED_SLOT_LIMIT    100U

/** @brief The time AO_Net has not been handed yet. */
#define SCHED_CHECK_UNSENT  (-2LL)

/** @brief The reason a round is answered busy while the scheduler holds work. */
#define SCHED_WHY_WORK      "sched work"

/** @brief The reason a round is answered busy when its wake time had no event to go in. */
#define SCHED_WHY_WAKE      "sched wake"

/** @brief The shortest wait the timer is armed for, milliseconds: a tick. */
#define SCHED_MIN_WAIT_MS   1U

/** @brief Milliseconds in a second, for the check's dwell. */
#define SCHED_MS_PER_S      1000LL

/** @brief The longest a time handed to AO_Net is kept for it once it has come,
 *         seconds: its minute and the check's dwell, rounded up. A report of
 *         the check that is lost (sched_check_done() refused) ends here. */
#define SCHED_CHECK_HOLD_S  ((int64_t)SCHED_CRON_SEC_MIN                 \
                             + (((int64_t)KODE_NET_CHECK_DWELL_MS         \
                                 + SCHED_MS_PER_S - 1LL) / SCHED_MS_PER_S))

/** @brief The console verb that takes the board to ASLEEP, ending the wake. */
#define SCHED_VERB_SLEEP     "sleep"

/** @brief The console verb that takes the board to OFF, ending the wake. */
#define SCHED_VERB_POWER_OFF "power_off"

/** @brief What is kept of an entry: the version and the three texts, nothing else. */
typedef struct {
    uint8_t version;                         /**< SCHED_STORE_VERSION.                     */
    char    name[KODE_SCHED_NAME_MAX + 1U];  /**< The entry's name; "mqtt" for the built-in. */
    char    when[KODE_SCHED_WHEN_MAX + 1U];  /**< The expression, or "off" for `mqtt`.      */
    char    cmd[KODE_SCHED_CMD_MAX];         /**< The line; empty for `mqtt`.              */
} SchedStored;

_Static_assert(sizeof(SchedStored) == SCHED_STORE_BYTES,
               "a stored record is its version and three texts, with no padding");
_Static_assert(KODE_SCHED_MAX <= SCHED_SLOT_LIMIT,
               "a slot is at most two digits: the key buffer is sized for it");
_Static_assert(KODE_SCHED_WHY_MAX > sizeof("unknown"),
               "the longest reason must fit an answer");

/** @brief One entry of the table. */
typedef struct {
    bool         used;                           /**< The slot holds an entry (`mqtt`: it is on). */
    uint8_t      owner;                          /**< A sched_owner_t.                          */
    bool         stored;                         /**< The record is in the store.               */
    bool         pending;                        /**< A console entry whose line has not gone out. */
    bool         ao_out;                         /**< An entry of code whose event is within the bound. */
    bool         warned;                         /**< A skipped minute has been said once.      */
    bool         late;                           /**< Its last line was given up on, unanswered. */
    bool         said;                           /**< The log said this fire's line goes to the console (not said again on a retry). */
    int64_t      late_min;                       /**< The minute (Unix seconds / 60) that line ran in. */
    sched_cron_t cron;                           /**< The parsed expression.                    */
    QActive     *target;                         /**< An entry of code: where each fire goes.   */
    enum_t       sig;                            /**< An entry of code: its signal.             */
    char         name[KODE_SCHED_NAME_MAX + 1U]; /**< The name.                                 */
    char         when[KODE_SCHED_WHEN_MAX + 1U]; /**< The expression as written.                */
    char         cmd[KODE_SCHED_CMD_MAX];        /**< A console entry's line.                   */
} SchedEntry;

/** @brief AO_Scheduler. */
typedef struct {
    QActive     super;          /**< The AO.                                            */
    QTimeEvt    due;            /**< The minute timer, one-shot.                        */
    QTimeEvt    work;           /**< The bound on the work of a minute, one-shot.       */
    QTimeEvt    retry;          /**< The wait before a line the console refused goes again, one-shot. */
    SchedEntry *tab;            /**< KODE_SCHED_MAX entries, in PSRAM; slot 0 is `mqtt`. */
    int64_t     last_min;       /**< The last minute (Unix seconds / 60) a pass ran for. */
    int64_t     armed_min;      /**< The minute the timer is armed for; SCHED_MINUTE_NONE for none. */
    int64_t     chk_sent;       /**< The time last handed to AO_Net; SCHED_CHECK_UNSENT for none yet. */
    uint8_t     out_slot;       /**< While a line is out: its entry's slot; SCHED_SLOT_NONE once that entry is gone. */
    bool        out_ends;       /**< The line out ends the wake (line_ends_wake()). */
    uint8_t     lp_who;         /**< The id lowpower_join() gave.                       */
    /* Nothing is posted to another AO before @c up: a post to an AO with no queue yet asserts. */
    bool        up;             /**< kode_init() said the system is up (SCHED_UP_SIG).  */
    bool        waiting_line;   /**< A line is out and the console has not answered.    */
    bool        work_on;        /**< The bound is running.                              */
    bool        lp_rest;        /**< The board sleeps (a round was answered ready).     */
    bool        timer_wake;     /**< The board woke for a timer and has not slept since. */
    bool        rest_asked;     /**< The board was asked to rest in this wake, and not refused for another reason than use. */
    bool        chk_owed;       /**< AO_Net could not be handed its time: try again.    */
} Scheduler;

/* cppcheck-suppress misra-c2012-8.9 ; the AO's one instance, used by its constructor and by the pointer the others post to */
static Scheduler l_sched;
QActive * const AO_Scheduler = &l_sched.super;

static QState Scheduler_initial(Scheduler * const me, void const * const par);
static QState Scheduler_on(Scheduler * const me, QEvt const * const e);
static QState Scheduler_waiting(Scheduler * const me, QEvt const * const e);

/* ---- Small helpers ------------------------------------------------------- */

/**
 * @brief The wall clock in whole seconds.
 *
 * @return Unix seconds, UTC; SCHED_NO_TIME while the clock is not set.
 */
static int64_t wall_s(void)
{
    int64_t secs = SCHED_NO_TIME;

    if ( khal_time_wall_known() )
    {
        uint64_t const whole = khal_time_wall_us() / SCHED_US_PER_S_U;

        secs = (int64_t)whole;
    }
    return secs;
}

/**
 * @brief Copy a text that must fit.
 *
 * @param dst Destination.
 * @param cap Its size.
 * @param src The text; NULL reads as empty.
 * @return false when @p src did not fit and @p dst holds the cut text.
 */
static bool copy_text(char *dst, size_t cap, const char *src)
{
    bool whole = true;

    dst[0] = '\0';
    if ( NULL != src )
    {
        int const n = snprintf(dst, cap, "%s", src);

        whole = (0 <= n) && ((size_t)n < cap);
    }
    return whole;
}

/**
 * @brief Whether @p c may be in an entry's name.
 *
 * Letters, digits, `_`, `-` and `.`: a name goes quoted into records and logs
 * and must not be able to break either.
 *
 * @param c The character.
 * @return true when it may.
 */
static bool sched_name_char(char c)
{
    return (('a' <= c) && ('z' >= c)) || (('A' <= c) && ('Z' >= c))
           || (('0' <= c) && ('9' >= c)) || ('_' == c) || ('-' == c)
           || ('.' == c);
}

/**
 * @brief Whether @p name is a legal name: 1 to KODE_SCHED_NAME_MAX of sched_name_char().
 *
 * @param name A NUL-terminated text.
 * @return true when it is.
 */
static bool name_valid(const char *name)
{
    size_t const len = strlen(name);
    bool ok = (0U < len) && (KODE_SCHED_NAME_MAX >= len);

    for ( size_t i = 0U; ok && (i < len); i++ )
    {
        ok = sched_name_char(name[i]);
    }
    return ok;
}

/**
 * @brief Whether an expression can ever match a minute.
 *
 * @param c A parsed expression.
 * @return true when sched_cron_next() finds a minute from the epoch.
 */
static bool can_match(const sched_cron_t *c)
{
    return sched_cron_next(c, SCHED_PROBE_FROM_S) >= 0;
}

/**
 * @brief Parse an expression for an entry.
 *
 * @param text The expression.
 * @param out  Receives the sets on success.
 * @return NULL when it is good; else the short static reason.
 */
static const char *when_check(const char *text, sched_cron_t *out)
{
    const char *why = NULL;

    if ( sched_cron_parse(text, out, &why) != KODE_OK )
    {
        if ( NULL == why )
        {
            why = "syntax";
        }
    }
    else if ( !can_match(out) )
    {
        why = "never";
    }
    else
    {
        why = NULL;
    }
    return why;
}

/**
 * @brief The reason a request's cut text is refused.
 *
 * @param flaw A SCHED_FLAW_*.
 * @return NULL for none, else the reason.
 */
static const char *flaw_why(uint8_t flaw)
{
    const char *why = NULL;

    switch ( flaw )
    {
        case (uint8_t)SCHED_FLAW_NAME: {
            why = "name";
            break;
        }
        case (uint8_t)SCHED_FLAW_WHEN: {
            why = "when";
            break;
        }
        case (uint8_t)SCHED_FLAW_CMD: {
            why = "cmd";
            break;
        }
        default: {
            why = NULL;
            break;
        }
    }
    return why;
}

/**
 * @brief The slot holding the entry called @p name.
 *
 * @param me   The AO.
 * @param name The name.
 * @return The slot, or SCHED_SLOT_NONE.
 */
static uint8_t slot_find(const Scheduler * const me, const char *name)
{
    uint8_t found = SCHED_SLOT_NONE;

    for ( uint8_t i = 0U; (SCHED_SLOT_NONE == found) && (KODE_SCHED_MAX > i); i++ )
    {
        if ( me->tab[i].used && (strcmp(me->tab[i].name, name) == 0) )
        {
            found = i;
        }
    }
    return found;
}

/**
 * @brief The first free slot after the built-in's.
 *
 * @param me The AO.
 * @return The slot, or SCHED_SLOT_NONE when the table is full.
 */
static uint8_t slot_free(const Scheduler * const me)
{
    uint8_t found = SCHED_SLOT_NONE;

    for ( uint8_t i = 1U; (SCHED_SLOT_NONE == found) && (KODE_SCHED_MAX > i); i++ )
    {
        if ( !me->tab[i].used )
        {
            found = i;
        }
    }
    return found;
}

/**
 * @brief The slot an entry made by @p owner goes in: its own if it has one, else a free one.
 *
 * @param me    The AO.
 * @param name  The entry's name.
 * @param owner Whose door is asking.
 * @param why   Receives "owner" when the name is another door's, or "full";
 *              untouched when there is a slot.
 * @return The slot; SCHED_SLOT_NONE when @p why was set.
 */
static uint8_t slot_for(const Scheduler * const me, const char *name,
                        sched_owner_t owner, const char **why)
{
    uint8_t at = slot_find(me, name);

    if ( SCHED_SLOT_NONE == at )
    {
        at = slot_free(me);
        if ( SCHED_SLOT_NONE == at )
        {
            *why = "full";
        }
    }
    else if ( (uint8_t)owner != me->tab[at].owner )
    {
        at = SCHED_SLOT_NONE;
        *why = "owner";
    }
    else
    {
        /* A change of the door's own entry: the same slot. */
    }
    return at;
}

/**
 * @brief Empty a slot.
 *
 * @param en The slot.
 */
static void entry_clear(SchedEntry * const en)
{
    (void)memset(en, 0, sizeof(*en));
    en->owner = (uint8_t)SCHED_BY_CONSOLE;
}

/**
 * @brief Whether an expression matches the minute that starts at @p minute_s.
 *
 * @param c        The expression.
 * @param minute_s Unix seconds of a minute's first second.
 * @return true when it does.
 */
static bool matches(const sched_cron_t *c, int64_t minute_s)
{
    return sched_cron_next(c, minute_s - 1) == minute_s;
}

/**
 * @brief Whether the clock stands KODE_SCHED_BACK_MAX_MIN minutes or more behind the minute last run.
 *
 * A clock that is a minute behind is a clock set back a little, and the
 * minute that ran must not run again. An hour or more behind is a clock set back
 * for good (a mistaken `time set` put right by SNTP, say), and waiting for it
 * to catch up would silence every entry for as long as it was wrong.
 *
 * @param me  The AO.
 * @param now The clock, Unix seconds; negative while it is not set.
 * @return true when it does.
 */
static bool clock_set_back(const Scheduler * const me, int64_t now)
{
    return (0 <= now) && (0 <= me->last_min)
           && ((me->last_min - (now / SCHED_CRON_SEC_MIN)) >= (int64_t)KODE_SCHED_BACK_MAX_MIN);
}

/**
 * @brief Forget the minutes run: the clock was set back for good.
 *
 * @param me       The AO.
 * @param now      The clock, Unix seconds.
 * @param last_min The minute to take for the last one run.
 */
static void clock_back_forget(Scheduler * const me, int64_t now, int64_t last_min)
{
    KLOG_N(TAG, "the clock was set back %lld minutes: entries run from the new time",
           (long long)(me->last_min - (now / SCHED_CRON_SEC_MIN)));
    me->last_min = last_min;
}

/**
 * @brief The first second an entry may next fire from: after the minute that last ran.
 *
 * @param me  The AO.
 * @param now The clock, Unix seconds.
 * @return The time to search after.
 */
static int64_t search_from(const Scheduler * const me, int64_t now)
{
    int64_t from = now;

    if ( (0 <= me->last_min) && !clock_set_back(me, now) )
    {
        int64_t const after_run = (me->last_min * SCHED_CRON_SEC_MIN)
                                  + (SCHED_CRON_SEC_MIN - 1);

        if ( after_run > from )
        {
            from = after_run;
        }
    }
    return from;
}

/* ---- Storage ----------------------------------------------------------------- */

/* An app reads and writes nothing here: the flash-write guard forbids it; entries live in RAM. */
#if CONFIG_KODEOS_BOOT_ROLE_OS
/**
 * @brief The key of a slot's record.
 *
 * @param slot The slot.
 * @param key  Receives it.
 * @param cap  Its size.
 */
static void store_key(uint8_t slot, char *key, size_t cap)
{
    if ( SCHED_SLOT_MQTT == slot )
    {
        (void)snprintf(key, cap, "%s", SCHED_KEY_MQTT);
    }
    else
    {
        (void)snprintf(key, cap, SCHED_KEY_FMT, (unsigned)slot);
    }
}
#endif

/**
 * @brief Write a slot's record.
 *
 * @param en   The entry.
 * @param slot Its slot.
 * @return true when it is in the store; false in an app, or when the store refused.
 */
static bool store_put(const SchedEntry *en, uint8_t slot)
{
    bool ok = false;

#if CONFIG_KODEOS_BOOT_ROLE_OS
    SchedStored rec;
    char key[SCHED_KEY_MAX];

    (void)memset(&rec, 0, sizeof(rec));
    rec.version = (uint8_t)SCHED_STORE_VERSION;
    (void)snprintf(rec.name, sizeof(rec.name), "%s", en->name);
    if ( en->used )
    {
        (void)snprintf(rec.when, sizeof(rec.when), "%s", en->when);
    }
    else
    {
        (void)snprintf(rec.when, sizeof(rec.when), "%s", KODE_SCHED_WHEN_OFF);
    }
    (void)snprintf(rec.cmd, sizeof(rec.cmd), "%s", en->cmd);
    store_key(slot, key, sizeof(key));
    ok = (khal_nvs_set_blob(SCHED_STORE_NS, key, &rec, sizeof(rec)) == KODE_OK);
    if ( !ok )
    {
        KLOG_W(TAG, "the entry '%s' could not be stored", en->name);
    }
#else
    (void)en;
    (void)slot;
#endif
    return ok;
}

/**
 * @brief Forget a slot's record.
 *
 * @param slot The slot.
 */
static void store_drop(uint8_t slot)
{
#if CONFIG_KODEOS_BOOT_ROLE_OS
    char key[SCHED_KEY_MAX];

    store_key(slot, key, sizeof(key));
    if ( khal_nvs_erase(SCHED_STORE_NS, key) != KODE_OK )
    {
        KLOG_W(TAG, "the record %s could not be forgotten", key);
    }
#else
    (void)slot;
#endif
}

/**
 * @brief Fill a slot with an entry whose expression has been checked.
 *
 * @param en    The slot.
 * @param owner Who makes it.
 * @param name  Its name.
 * @param when  Its expression as written.
 * @param cron  The parsed expression.
 * @param cmd   A console entry's line; NULL for none.
 */
static void entry_fill(SchedEntry * const en, sched_owner_t owner,
                       const char *name, const char *when,
                       const sched_cron_t *cron, const char *cmd)
{
    entry_clear(en);
    en->used  = true;
    en->owner = (uint8_t)owner;
    en->cron  = *cron;
    (void)copy_text(en->name, sizeof(en->name), name);
    (void)copy_text(en->when, sizeof(en->when), when);
    (void)copy_text(en->cmd, sizeof(en->cmd), cmd);
}

#if CONFIG_KODEOS_BOOT_ROLE_OS
/**
 * @brief Whether a record read back can be trusted, and what it says.
 *
 * @param rec  The record.
 * @param len  How many bytes were stored.
 * @param cron Receives the expression when it is not "off".
 * @param off  Receives true for "off".
 * @return NULL when it is good; else why not.
 */
static const char *record_check(const SchedStored *rec, size_t len,
                                sched_cron_t *cron, bool *off)
{
    const char *why = NULL;

    *off = false;
    if ( sizeof(*rec) != len )
    {
        why = "another layout";
    }
    else if ( SCHED_STORE_VERSION != rec->version )
    {
        why = "another version";
    }
    else if ( (memchr(rec->name, 0, sizeof(rec->name)) == NULL)
             || (memchr(rec->when, 0, sizeof(rec->when)) == NULL)
             || (memchr(rec->cmd, 0, sizeof(rec->cmd)) == NULL) )
    {
        why = "not terminated";
    }
    else if ( !name_valid(rec->name) )
    {
        why = "name";
    }
    else if ( strcmp(rec->when, KODE_SCHED_WHEN_OFF) == 0 )
    {
        *off = true;
    }
    else
    {
        why = when_check(rec->when, cron);
    }
    return why;
}

/**
 * @brief Put one record read from the store into its slot, or skip it.
 *
 * @param me   The AO.
 * @param slot The slot the key names.
 * @param key  The key, for the log.
 * @param rec  The record.
 * @param len  How many bytes were stored.
 */
static void store_take(Scheduler * const me, uint8_t slot, const char *key,
                       const SchedStored *rec, size_t len)
{
    sched_cron_t cron;
    bool off = false;
    const char *why = record_check(rec, len, &cron, &off);

    if ( (NULL == why) && (SCHED_SLOT_MQTT == slot)
        && (strcmp(rec->name, KODE_SCHED_MQTT_NAME) != 0) )
    {
        why = "name";
    }
    if ( (NULL == why) && (SCHED_SLOT_MQTT != slot)
        && (('\0' == rec->cmd[0]) || off
            || (strcmp(rec->name, KODE_SCHED_MQTT_NAME) == 0)
            || (slot_find(me, rec->name) != SCHED_SLOT_NONE)) )
    {
        why = "entry";
    }
    if ( NULL != why )
    {
        KLOG_W(TAG, "the stored record %s is skipped: %s", key, why);
    }
    else if ( SCHED_SLOT_MQTT == slot )
    {
        if ( off )
        {
            entry_clear(&me->tab[slot]);
            (void)snprintf(me->tab[slot].name, sizeof(me->tab[slot].name),
                           "%s", KODE_SCHED_MQTT_NAME);
            me->tab[slot].owner = (uint8_t)SCHED_BY_BUILTIN;
        }
        else
        {
            entry_fill(&me->tab[slot], SCHED_BY_BUILTIN, rec->name, rec->when,
                       &cron, NULL);
        }
        me->tab[slot].stored = true;
    }
    else
    {
        entry_fill(&me->tab[slot], SCHED_BY_CONSOLE, rec->name, rec->when,
                   &cron, rec->cmd);
        me->tab[slot].stored = true;
    }
}
#endif

/**
 * @brief Read the store into the table: `mqtt` first, then the console entries.
 *
 * Nothing in an app. A record that fails is skipped with a WARN; `mqtt` then
 * keeps its default.
 *
 * @param me The AO.
 */
static void store_load(Scheduler * const me)
{
#if CONFIG_KODEOS_BOOT_ROLE_OS
    SchedStored rec;
    char key[SCHED_KEY_MAX];

    for ( uint8_t slot = 0U; KODE_SCHED_MAX > slot; slot++ )
    {
        size_t len = sizeof(rec);

        store_key(slot, key, sizeof(key));
        (void)memset(&rec, 0, sizeof(rec));
        kode_status_t const got = khal_nvs_get_blob(SCHED_STORE_NS, key, &rec, &len);

        if ( KODE_OK == got )
        {
            store_take(me, slot, key, &rec, len);
        }
        else if ( KODE_DENIED != got )
        {
            /* Denied is "nothing stored"; any other answer is a record the
             * store would not give back (one too big for this layout, say). */
            KLOG_W(TAG, "the stored record %s is skipped: unreadable", key);
        }
        else
        {
            /* Nothing stored under this key. */
        }
    }
#else
    (void)me;
#endif
}

/* ---- Answers ------------------------------------------------------------------ */

/** @brief An answer in the making: its end is reserved before any entry goes. */
typedef struct {
    QActive     *who;      /**< Who asked; NULL for nobody.                     */
    SchedEndEvt *end;      /**< The end, allocated first so that it exists.     */
    uint8_t      count;    /**< Entries that reached the asker.                 */
    bool         dropped;  /**< An entry could not be sent.                     */
} Reply;

/**
 * @brief Begin an answer: reserve its end.
 *
 * @param r   The answer.
 * @param who Who asked; NULL for nobody.
 */
static void reply_open(Reply * const r, QActive * const who)
{
    r->who     = who;
    r->end     = NULL;
    r->count   = 0U;
    r->dropped = false;
    if ( NULL != who )
    {
        r->end = Q_NEW_X(SchedEndEvt, 0U, SCHED_END_SIG);
        if ( NULL == r->end )
        {
            KLOG_W(TAG, "out of events; the request cannot be answered");
        }
    }
}

/**
 * @brief Send one entry to the asker.
 *
 * Posted with KODE_SCHED_END_MARGIN slots left free in the asker's queue, so
 * that the end of the answer has room: a console fed from the channel and an
 * app with a short queue must refuse, never assert, and never lose the end.
 *
 * @param me The AO.
 * @param r  The answer.
 * @param en The entry.
 */
static void reply_entry(const Scheduler * const me, Reply * const r,
                        const SchedEntry * const en)
{
    if ( NULL != r->who )
    {
        SchedEntryEvt * const e = Q_NEW_X(SchedEntryEvt, 0U, SCHED_ENTRY_SIG);

        if ( NULL == e )
        {
            KLOG_W(TAG, "out of events; an entry was dropped from an answer");
            r->dropped = true;
        }
        else
        {
            int64_t const now = wall_s();

            e->owner  = en->owner;
            e->stored = en->stored;
            e->next_s = SCHED_NO_TIME;
            if ( (0 <= now) && en->used )
            {
                e->next_s = sched_cron_next(&en->cron, search_from(me, now));
            }
            (void)copy_text(e->name, sizeof(e->name), en->name);
            /* A built-in that is switched off is still shown, as "off". */
            if ( en->used )
            {
                (void)copy_text(e->when, sizeof(e->when), en->when);
            }
            else
            {
                (void)copy_text(e->when, sizeof(e->when), KODE_SCHED_WHEN_OFF);
            }
            (void)copy_text(e->cmd, sizeof(e->cmd), en->cmd);
            /* A refused post recycles the event itself. */
            if ( QACTIVE_POST_X(r->who, &e->super, KODE_SCHED_END_MARGIN, NULL) )
            {
                r->count++;
            }
            else
            {
                KLOG_W(TAG, "an entry was dropped from an answer: the asker's queue is full");
                r->dropped = true;
            }
        }
    }
}

/**
 * @brief End an answer.
 *
 * KODE_OK; KODE_DENIED with @p why; or, when an entry did not arrive,
 * KODE_ERROR with "partial" and the count that did. The end is posted with a
 * margin of none: the entries left a slot for it.
 *
 * @param r   The answer.
 * @param why NULL for success; else the reason it was refused.
 */
static void reply_close(Reply * const r, const char *why)
{
    SchedEndEvt * const e = r->end;

    if ( NULL != e )
    {
        e->st    = (uint8_t)KODE_OK;
        e->count = r->count;
        e->why[0] = '\0';
        if ( NULL != why )
        {
            e->st = (uint8_t)KODE_DENIED;
            (void)snprintf(e->why, sizeof(e->why), "%s", why);
        }
        else if ( r->dropped )
        {
            e->st = (uint8_t)KODE_ERROR;
            (void)snprintf(e->why, sizeof(e->why), "%s", "partial");
        }
        else
        {
            /* Every entry arrived. */
        }
        if ( !QACTIVE_POST_X(r->who, &e->super, 0U, NULL) )
        {
            KLOG_E(TAG, "the end of an answer was dropped: the asker's queue is full");
        }
        r->end = NULL;
    }
}

/* ---- The line whose answer is owed ------------------------------------------------- */

/**
 * @brief Whether the console owes the answer to a line of this slot's entry.
 *
 * @param me   The AO.
 * @param slot The slot.
 * @return true when the line out is this entry's.
 */
static bool line_is_out(const Scheduler * const me, uint8_t slot)
{
    return me->waiting_line && (me->out_slot == slot);
}

#if CONFIG_KODEOS_CONSOLE
/**
 * @brief Whether a word separator of the console's: a space or a tab.
 *
 * @param c The character.
 * @return true when it is.
 */
static bool line_blank(char c)
{
    return (' ' == c) || ('\t' == c);
}

/**
 * @brief Whether a line's verb is one that ends the wake: `sleep` or
 *        `power_off`, the two the console answers only once the board has
 *        changed state (KCLI_LOWPOWER).
 *
 * @param line The console line.
 * @return true when its first word is one of them.
 */
static bool line_ends_wake(const char *line)
{
    size_t at = 0U;
    size_t len = 0U;
    bool ends = false;

    while ( line_blank(line[at]) )
    {
        at++;
    }
    while ( ('\0' != line[at + len]) && (!line_blank(line[at + len])) )
    {
        len++;
    }
    if ( ((sizeof(SCHED_VERB_SLEEP) - 1U) == len)
        && (strncmp(&line[at], SCHED_VERB_SLEEP, len) == 0) )
    {
        ends = true;
    }
    else if ( ((sizeof(SCHED_VERB_POWER_OFF) - 1U) == len)
             && (strncmp(&line[at], SCHED_VERB_POWER_OFF, len) == 0) )
    {
        ends = true;
    }
    else
    {
        /* Any other verb leaves the wake as it is. */
    }
    return ends;
}
#endif

/**
 * @brief Whether an entry still has the work of an earlier minute out.
 *
 * @param me   The AO.
 * @param slot The entry's slot.
 * @return true when its line is not gone out or not answered, or its event is
 *         within the bound.
 */
static bool entry_busy(const Scheduler * const me, uint8_t slot)
{
    return me->tab[slot].pending || me->tab[slot].ao_out || line_is_out(me, slot);
}

/**
 * @brief Whether the work of the minutes that fired is done.
 *
 * @param me The AO.
 * @return true when no line waits for the console and no entry has work out
 *         that the bound has not ended.
 */
static bool work_done(const Scheduler * const me)
{
    bool done = (!me->waiting_line) || (NULL == me->tab);

    for ( uint8_t i = 0U; done && (NULL != me->tab) && (KODE_SCHED_MAX > i); i++ )
    {
        if ( me->tab[i].used && (me->tab[i].pending || me->tab[i].ao_out) )
        {
            done = false;
        }
    }
    return done;
}

#if CONFIG_KODE_LOWPOWER
/**
 * @brief Whether the scheduler itself still holds work: a line it has not
 *        sent, unless the line out ends the wake, or an event of code within
 *        the bound.
 *
 * A line handed to the console is the console's to answer for. Lines behind
 * `sleep` or `power_off` are not held: that line is answered only after the
 * round it asks for, which holding them would refuse. Behind any other line
 * they are held. Without a table nothing is held.
 *
 * @param me The AO.
 * @return true when it does.
 */
static bool work_held(const Scheduler * const me)
{
    bool const behind_end = me->waiting_line && me->out_ends;
    bool held = false;

    for ( uint8_t i = 0U; (!held) && (NULL != me->tab) && (KODE_SCHED_MAX > i); i++ )
    {
        SchedEntry const * const en = &me->tab[i];

        held = en->used && (en->ao_out || (en->pending && (!behind_end)));
    }
    return held;
}
#endif

/**
 * @brief Drop the lines of the minute that have not gone out.
 *
 * @param me  The AO.
 * @param why Words for the WARN of each: why it did not run.
 */
static void lines_drop(Scheduler * const me, const char *why)
{
    for ( uint8_t i = 0U; (NULL != me->tab) && (KODE_SCHED_MAX > i); i++ )
    {
        SchedEntry * const en = &me->tab[i];

        if ( en->used && en->pending )
        {
            en->pending = false;
            KLOG_W(TAG, "the entry '%s' did not run: %s", en->name, why);
        }
    }
}

#if CONFIG_KODE_LOWPOWER
/**
 * @brief Whether a refusal of the rest passes by itself soon, so that the next
 *        pass may ask again.
 *
 * @param why The refusal, "who: why".
 * @return true for KODE_LOWPOWER_WHY_PENDING and KODE_LOWPOWER_WHY_RUNNING.
 */
static bool rest_passes(const char *why)
{
    return (strcmp(why, KODE_LOWPOWER_WHY_PENDING) == 0)
           || (strcmp(why, KODE_LOWPOWER_WHY_RUNNING) == 0);
}
#endif

/**
 * @brief Ask the board to rest when it woke for a timer and the work of the
 *        minutes that ran is done.
 *
 * Called after each pass and at the end of its work; asked once, no answer
 * waited for. A refusal that passes by itself (rest_passes()) lets the next
 * pass ask again (LOWPOWER_REFUSED_SIG); any other stands for the wake, and
 * the board sleeps at its idle time.
 *
 * @param me The AO.
 */
static void rest_try(Scheduler * const me)
{
    if ( me->timer_wake && (!me->rest_asked) && (!me->lp_rest) && work_done(me) )
    {
        me->rest_asked = true;
        if ( !lowpower_request_rest(KODE_LOWPOWER_SRC_SCHED) )
        {
            KLOG_W(TAG, "the request to rest was not posted; the board sleeps at its idle time");
        }
    }
}

/**
 * @brief Look at whether the work is done: end the bound, and ask for rest.
 *
 * @param me The AO.
 */
static void work_settle(Scheduler * const me)
{
    if ( work_done(me) )
    {
        if ( me->work_on )
        {
            (void)QTimeEvt_disarm(&me->work);
            me->work_on = false;
        }
        rest_try(me);
    }
}

/**
 * @brief Start the bound on the work that just went out.
 *
 * @param me The AO.
 */
static void work_arm(Scheduler * const me)
{
    (void)QTimeEvt_disarm(&me->work);
    QTimeEvt_armX(&me->work, KQP_TICKS_PER_MS(KODE_SCHED_WORK_BOUND_MS), 0U);
    me->work_on = true;
}

/**
 * @brief The bound ran out: give up on what is still out.
 *
 * Lines not yet out are dropped, the wait for the console ends, and an event
 * of code is taken as handled. An unanswered line is forgotten, so a lost
 * answer cannot end the wait for a later one: its entry is marked late and
 * its next minute is skipped.
 *
 * @param me The AO.
 */
static void work_bound(Scheduler * const me)
{
    me->work_on = false;
    (void)QTimeEvt_disarm(&me->retry);
    if ( me->waiting_line )
    {
        KLOG_W(TAG, "a line did not answer in %u ms; the work of its minute is given up",
               (unsigned)KODE_SCHED_WORK_BOUND_MS);
        if ( SCHED_SLOT_NONE != me->out_slot )
        {
            SchedEntry * const en = &me->tab[me->out_slot];

            en->late     = true;
            en->late_min = me->last_min;
        }
    }
    lines_drop(me, "the work of its minute ran out of time");
    for ( uint8_t i = 0U; KODE_SCHED_MAX > i; i++ )
    {
        me->tab[i].ao_out = false;
    }
    me->out_slot     = SCHED_SLOT_NONE;
    me->out_ends     = false;
    me->waiting_line = false;
    work_settle(me);
}

/* ---- The check handed to AO_Net ----------------------------------------------------- */

/**
 * @brief The time to hand AO_Net: the `mqtt` entry's next minute after the clock.
 *
 * @param me  The AO.
 * @param now The clock, Unix seconds; negative while it is not set.
 * @return Unix seconds; -1 while the entry is off or the clock is not set.
 */
static int64_t chk_want(const Scheduler * const me, int64_t now)
{
    int64_t want = SCHED_NO_TIME;

    if ( (0 <= now) && me->tab[SCHED_SLOT_MQTT].used )
    {
        want = sched_cron_next(&me->tab[SCHED_SLOT_MQTT].cron, now);
    }
    return want;
}

/**
 * @brief Hand AO_Net a time, and remember whether it took it.
 *
 * @param me   The AO.
 * @param want The time, Unix seconds; -1 for none.
 */
static void chk_send(Scheduler * const me, int64_t want)
{
#if CONFIG_KODEOS_NET && CONFIG_KODE_LOWPOWER
    bool posted = true;   /* without AO_Net linked there is no radio to wake */

    if ( kaos_linked(&kaos_net_start) )
    {
        posted = net_check_at(want);
    }
#else
    bool const posted = true;   /* there is no AO_Net to wake the radio */
#endif

    me->chk_owed = !posted;
    if ( posted )
    {
        me->chk_sent = want;
    }
}

/**
 * @brief Whether the time AO_Net was handed has come, AO_Net has not told the
 *        check made (sched_check_done()), and SCHED_CHECK_HOLD_S have not
 *        passed since it came.
 *
 * Until then the time is AO_Net's: a later one would cut its dwell short and
 * the check would never be made. The hold is bounded, so a lost report costs
 * one check, not every later one.
 *
 * @param me  The AO.
 * @param now The clock, Unix seconds; negative while it is not set.
 * @return true while it is held.
 */
static bool chk_begun(const Scheduler * const me, int64_t now)
{
    return (0 <= me->chk_sent) && (now >= me->chk_sent)
           && ((me->chk_sent + SCHED_CHECK_HOLD_S) > now);
}

/**
 * @brief Keep AO_Net's check time where the `mqtt` entry says.
 *
 * Nothing goes before the system is up (a post to an AO with no queue
 * asserts), nor asleep, except to answer a check made. A time that has come is
 * kept until the check is made or SCHED_CHECK_HOLD_S pass (chk_begun()); the
 * entry switched off or an unset clock still take it back.
 *
 * @param me    The AO.
 * @param force Send even asleep (the answer to a check made).
 */
static void chk_sync(Scheduler * const me, bool force)
{
    if ( (NULL != me->tab) && me->up && (force || (!me->lp_rest)) )
    {
        int64_t const now  = wall_s();
        int64_t const want = chk_want(me, now);

        if ( chk_begun(me, now) && (0 <= want) )
        {
            /* AO_Net's check is due and not made: it keeps its time. */
        }
        else if ( me->chk_owed || (want != me->chk_sent) )
        {
            chk_send(me, want);
        }
        else
        {
            /* AO_Net has the time already. */
        }
    }
}

/* ---- The timer -------------------------------------------------------------------- */

/**
 * @brief Whether an entry has work to do while the board is awake.
 *
 * @param en The slot.
 * @return true for a console entry or an entry of code; `mqtt` does none awake.
 */
static bool entry_has_work(const SchedEntry *en)
{
    return en->used && ((uint8_t)SCHED_BY_BUILTIN != en->owner);
}

/**
 * @brief The next minute an entry falls due in, searching after @p from.
 *
 * A late entry skips the minute right after its unanswered line's (fire_pass()).
 * For a wake time the search passes over that minute, so the board is not
 * woken for a pass that will fire nothing.
 *
 * @param en   The entry.
 * @param from Unix seconds to search after.
 * @param wake The minute is a wake time: pass over a late entry's skip.
 * @return Unix seconds of the minute, or -1 for none.
 */
static int64_t entry_next(const SchedEntry *en, int64_t from, bool wake)
{
    int64_t next = sched_cron_next(&en->cron, from);

    if ( wake && en->late && (0 <= next)
        && ((next / SCHED_CRON_SEC_MIN) == (en->late_min + 1)) )
    {
        next = sched_cron_next(&en->cron, next);
    }
    return next;
}

/**
 * @brief Arm the timer for the earliest next minute, or ask the clock again.
 *
 * Worked out from the clock as it is now: nothing is owed for minutes a jump
 * skipped. Before the system is up the minute is worked out but not armed:
 * SCHED_UP_SIG arms it.
 *
 * @param me   The AO.
 * @param wake The minute is the wake time a round asks for (entry_next()).
 */
static void timer_arm_for(Scheduler * const me, bool wake)
{
    int64_t const now = wall_s();
    int64_t const kept = me->armed_min;
    bool work = false;
    uint32_t delay_ms = KODE_SCHED_CLOCK_POLL_MS;

    (void)QTimeEvt_disarm(&me->due);
    me->armed_min = SCHED_MINUTE_NONE;
    if ( clock_set_back(me, now) )
    {
        clock_back_forget(me, now, now / SCHED_CRON_SEC_MIN);
    }
    if ( NULL != me->tab )
    {
        int64_t from = search_from(me, now);
        int64_t earliest = SCHED_NO_TIME;

        /* A request handled while the timer's own minute has begun and its
         * expiry is still to come must not take that minute away: search
         * from just before it. */
        if ( (0 <= now) && (SCHED_MINUTE_NONE != kept) && (kept > me->last_min)
            && ((kept * SCHED_CRON_SEC_MIN) <= now) )
        {
            from = (kept * SCHED_CRON_SEC_MIN) - 1;
        }

        for ( uint8_t i = 0U; KODE_SCHED_MAX > i; i++ )
        {
            if ( entry_has_work(&me->tab[i]) )
            {
                work = true;
                if ( 0 <= now )
                {
                    int64_t const next = entry_next(&me->tab[i], from, wake);

                    if ( (0 <= next) && ((0 > earliest) || (next < earliest)) )
                    {
                        earliest = next;
                    }
                }
            }
        }
        if ( work && (0 <= now) )
        {
            work = (0 <= earliest);
            if ( work )
            {
                int64_t const wait_us = (earliest * SCHED_US_PER_S)
                                        - (int64_t)khal_time_wall_us();
                /* Rounded up: a timer must not be the one that is early. */
                int64_t const wait_ms = (wait_us + SCHED_US_PER_MS - 1) / SCHED_US_PER_MS;

                me->armed_min = earliest / SCHED_CRON_SEC_MIN;
                delay_ms = KODE_SCHED_ARM_MAX_MS;
                if ( (int64_t)KODE_SCHED_ARM_MAX_MS > wait_ms )
                {
                    delay_ms = SCHED_MIN_WAIT_MS;
                    if ( (int64_t)SCHED_MIN_WAIT_MS < wait_ms )
                    {
                        delay_ms = (uint32_t)wait_ms;
                    }
                }
            }
        }
        /* The `mqtt` entry has no work here, but AO_Net is handed its time
         * once the clock is known: the clock is asked until it is. */
        if ( me->tab[SCHED_SLOT_MQTT].used && (0 > now)
            && ((!work) || (KODE_SCHED_CLOCK_POLL_MS < delay_ms)) )
        {
            work     = true;
            delay_ms = KODE_SCHED_CLOCK_POLL_MS;
        }
    }
    if ( work && me->up )
    {
        QTimeEvt_armX(&me->due, (QTimeEvtCtr)KQP_TICKS_PER_MS(delay_ms), 0U);
    }
    chk_sync(me, false);
}

/**
 * @brief Arm the timer for the earliest next minute while the board is awake.
 *
 * @param me The AO.
 */
static void timer_arm(Scheduler * const me)
{
    timer_arm_for(me, false);
}

/**
 * @brief SCHED_WALL_SET_SIG: the clock was written, so arm the timer from it.
 *
 * The minute the timer was armed for was worked out on the clock that is gone,
 * and the arming would otherwise keep it as a minute begun and not yet run.
 * A board at rest arms the timer when it wakes.
 *
 * @param me The AO.
 */
static void on_wall_set(Scheduler * const me)
{
    me->armed_min = SCHED_MINUTE_NONE;
    if ( !me->lp_rest )
    {
        timer_arm(me);
    }
}

/**
 * @brief Post an entry of code its event.
 *
 * @param en The entry.
 * @return true when the event was posted.
 */
static bool fire_ao(const SchedEntry * const en)
{
    SchedFireEvt * const e = Q_NEW_X(SchedFireEvt, 0U, en->sig);
    bool posted = false;

    if ( NULL == e )
    {
        KLOG_W(TAG, "out of events; the entry '%s' did not fire", en->name);
    }
    else
    {
        (void)copy_text(e->name, sizeof(e->name), en->name);
        posted = QACTIVE_POST_X(en->target, &e->super, KODE_SCHED_POST_MARGIN, NULL);
        if ( !posted )
        {
            KLOG_W(TAG, "the entry '%s' did not fire: its AO's queue is full",
                   en->name);
        }
        else
        {
            KLOG_I(TAG, "entry '%s' fired", en->name);
        }
    }
    return posted;
}

/**
 * @brief Mark what fires in a minute: lines wait their turn, events go now.
 *
 * An entry whose work from an earlier minute is still out is skipped, and
 * that is said once until it fires again.
 *
 * @param me        The AO.
 * @param minute_s  Unix seconds of the minute's first second.
 * @return true when some work is out that the bound must wait for.
 */
static bool fire_pass(Scheduler * const me, int64_t minute_s)
{
    bool out = false;

    for ( uint8_t i = 0U; KODE_SCHED_MAX > i; i++ )
    {
        SchedEntry * const en = &me->tab[i];

        if ( entry_has_work(en) && matches(&en->cron, minute_s) )
        {
            bool skip = entry_busy(me, i);

            if ( (!skip) && en->late )
            {
                skip = ((minute_s / SCHED_CRON_SEC_MIN) == (en->late_min + 1));
                en->late = false;
            }
            if ( skip )
            {
                if ( !en->warned )
                {
                    en->warned = true;
                    KLOG_W(TAG, "the entry '%s' is skipped: its last run is not done",
                           en->name);
                }
            }
            else
            {
                en->warned = false;
                if ( (uint8_t)SCHED_BY_CONSOLE == en->owner )
                {
                    en->pending = true;
                    en->said    = false;
                    out = true;
                }
                else
                {
                    en->ao_out = fire_ao(en);
                    out = out || en->ao_out;
                }
            }
        }
    }
    return out;
}

/**
 * @brief Give the console the next pending line.
 *
 * Takes pending entries in table order until one is posted. A line that
 * cannot be posted is dropped with a WARN, so a console that is behind costs
 * the minute and nothing more.
 *
 * @param me The AO.
 * @return true when a line went out and the console owes its answer.
 */
static bool line_send(Scheduler * const me)
{
    bool sent = false;

    for ( uint8_t i = 0U; !sent && (KODE_SCHED_MAX > i); i++ )
    {
        SchedEntry * const en = &me->tab[i];

        if ( en->used && en->pending )
        {
            en->pending = false;
#if CONFIG_KODEOS_CONSOLE
            {
                ConsoleLineEvt * const e = Q_NEW_X(ConsoleLineEvt, 0U, CONSOLE_LINE_SIG);

                if ( NULL == e )
                {
                    KLOG_W(TAG, "out of events; the entry '%s' did not run", en->name);
                }
                else
                {
                    e->src = (uint8_t)KODE_CONSOLE_SRC_SCHED;
                    (void)copy_text(e->line, sizeof(e->line), en->cmd);
                    /* A refused post recycles the event itself. */
                    if ( QACTIVE_POST_X(AO_Console, &e->super, KODE_SCHED_POST_MARGIN,
                                       &me->super) )
                    {
                        /* Said once a fire: a retry after a busy answer is
                         * silent. Only the hand-over is said. */
                        if ( !en->said )
                        {
                            en->said = true;
                            KLOG_I(TAG, "entry '%s' is due; its line goes to the console",
                                   en->name);
                        }
                        /* A line in hand needs no retry: its answer decides. */
                        (void)QTimeEvt_disarm(&me->retry);
                        me->out_slot = i;
                        me->out_ends = line_ends_wake(en->cmd);
                        sent = true;
                    }
                    else
                    {
                        KLOG_W(TAG, "the entry '%s' did not run: the console is busy",
                               en->name);
                    }
                }
            }
#else
            KLOG_W(TAG, "the entry '%s' did not run: this build has no console", en->name);
#endif
        }
    }
    return sent;
}

/**
 * @brief The timer expired: run the pass for the minute the clock says.
 *
 * In the first KODE_SCHED_SETTLE_MS after boot the pass waits for the window
 * to end and then runs if the clock is still in the minute (a late wake inside
 * the same minute still runs it); the minute the timer was armed for is kept.
 *
 * @param me The AO.
 * @return true when a line went out, so the AO now waits for its answer.
 */
static bool timer_due(Scheduler * const me)
{
    int64_t const now = wall_s();
    uint32_t const up_ms = khal_time_ms();
    int64_t minute = SCHED_NO_TIME;
    bool sent = false;
    bool settling = false;
    bool pass_due = false;

    if ( 0 <= now )
    {
        minute = now / SCHED_CRON_SEC_MIN;
        pass_due = (minute >= me->armed_min);
    }
    if ( pass_due )
    {
        if ( KODE_SCHED_SETTLE_MS > up_ms )
        {
            settling = true;
            (void)QTimeEvt_disarm(&me->due);
            QTimeEvt_armX(&me->due, (QTimeEvtCtr)KQP_TICKS_PER_MS(KODE_SCHED_SETTLE_MS - up_ms), 0U);
        }
        else
        {
            if ( fire_pass(me, minute * SCHED_CRON_SEC_MIN) )
            {
                work_arm(me);
            }
            me->last_min = minute;
            if ( !me->waiting_line )
            {
                sent = line_send(me);
            }
            if ( !sent )
            {
                work_settle(me);
            }
        }
    }
    if ( !settling )
    {
        timer_arm(me);
    }
    return sent;
}

/* ---- Requests ------------------------------------------------------------------- */

/**
 * @brief Make or change `mqtt`.
 *
 * @param me   The AO.
 * @param when The new expression, or "off".
 * @param cmd  Must be empty.
 * @param slot Receives the slot to answer with; SCHED_SLOT_NONE when it is off.
 * @return NULL on success; else why not.
 */
static const char *mqtt_set(Scheduler * const me, const char *when,
                            const char *cmd, uint8_t *slot)
{
    const char *why = NULL;
    SchedEntry * const en = &me->tab[SCHED_SLOT_MQTT];
    sched_cron_t cron;

    *slot = SCHED_SLOT_NONE;
    if ( '\0' != cmd[0] )
    {
        why = "cmd";
    }
    else if ( strcmp(when, KODE_SCHED_WHEN_OFF) == 0 )
    {
        entry_clear(en);
        (void)snprintf(en->name, sizeof(en->name), "%s", KODE_SCHED_MQTT_NAME);
        en->owner  = (uint8_t)SCHED_BY_BUILTIN;
        en->stored = store_put(en, SCHED_SLOT_MQTT);
    }
    else
    {
        why = when_check(when, &cron);
        if ( NULL == why )
        {
            entry_fill(en, SCHED_BY_BUILTIN, KODE_SCHED_MQTT_NAME, when, &cron, NULL);
            en->stored = store_put(en, SCHED_SLOT_MQTT);
            *slot = SCHED_SLOT_MQTT;
        }
    }
    return why;
}

/**
 * @brief Remove a console entry.
 *
 * @param me   The AO.
 * @param name Its name.
 * @return NULL on success; else why not.
 */
static const char *console_remove(Scheduler * const me, const char *name)
{
    const char *why = "unknown";
    uint8_t const slot = slot_find(me, name);

    if ( SCHED_SLOT_NONE != slot )
    {
        if ( (uint8_t)SCHED_BY_CONSOLE == me->tab[slot].owner )
        {
            if ( line_is_out(me, slot) )
            {
                /* The console still answers it; the answer is nobody's now. */
                me->out_slot = SCHED_SLOT_NONE;
            }
            entry_clear(&me->tab[slot]);
            store_drop(slot);
            why = NULL;
        }
        else
        {
            why = "owner";
        }
    }
    return why;
}

/**
 * @brief Make or change a console entry.
 *
 * @param me   The AO.
 * @param r    The request.
 * @param slot Receives the slot to answer with; SCHED_SLOT_NONE for none.
 * @return NULL on success; else why not.
 */
static const char *console_set(Scheduler * const me, const SchedSetReqEvt *r,
                               uint8_t *slot)
{
    const char *why = flaw_why(r->flaw);
    sched_cron_t cron;

    *slot = SCHED_SLOT_NONE;
    if ( NULL != why )
    {
        /* Cut text: refused as it is. */
    }
    else if ( !name_valid(r->name) )
    {
        why = "name";
    }
    else if ( strcmp(r->name, KODE_SCHED_MQTT_NAME) == 0 )
    {
        why = mqtt_set(me, r->when, r->cmd, slot);
    }
    else if ( strcmp(r->when, KODE_SCHED_WHEN_OFF) == 0 )
    {
        why = console_remove(me, r->name);
    }
    else
    {
        why = when_check(r->when, &cron);
        if ( (NULL == why) && ('\0' == r->cmd[0]) )
        {
            why = "cmd";
        }
        if ( NULL == why )
        {
            uint8_t const at = slot_for(me, r->name, SCHED_BY_CONSOLE, &why);

            if ( NULL == why )
            {
                entry_fill(&me->tab[at], SCHED_BY_CONSOLE, r->name, r->when, &cron, r->cmd);
                me->tab[at].stored = store_put(&me->tab[at], at);
                *slot = at;
            }
        }
    }
    return why;
}

/**
 * @brief Make or change an entry of code.
 *
 * @param me   The AO.
 * @param r    The request.
 * @param slot Receives the slot to answer with; SCHED_SLOT_NONE for none.
 * @return NULL on success; else why not.
 */
static const char *code_set(Scheduler * const me, const SchedSetAoReqEvt *r,
                            uint8_t *slot)
{
    const char *why = flaw_why(r->flaw);
    sched_cron_t cron;

    *slot = SCHED_SLOT_NONE;
    if ( NULL != why )
    {
        /* Cut text: refused as it is. */
    }
    else if ( !name_valid(r->name) )
    {
        why = "name";
    }
    else if ( strcmp(r->name, KODE_SCHED_MQTT_NAME) == 0 )
    {
        why = "owner";
    }
    else if ( NULL == r->target )
    {
        why = "args";
    }
    else
    {
        why = when_check(r->when, &cron);
        if ( NULL == why )
        {
            uint8_t const at = slot_for(me, r->name, SCHED_BY_CODE, &why);

            if ( NULL == why )
            {
                entry_fill(&me->tab[at], SCHED_BY_CODE, r->name, r->when, &cron, NULL);
                me->tab[at].target = r->target;
                me->tab[at].sig    = r->fire_sig;
                *slot = at;
            }
        }
    }
    return why;
}

/**
 * @brief Answer a set: the entry as it is, then the end.
 *
 * @param me   The AO.
 * @param who  Who asked.
 * @param why  NULL when it was taken.
 * @param slot The entry's slot, or SCHED_SLOT_NONE for none to show.
 */
static void set_answer(Scheduler * const me, QActive * const who,
                       const char *why, uint8_t slot)
{
    Reply r;

    reply_open(&r, who);
    if ( (NULL == why) && (SCHED_SLOT_NONE != slot) )
    {
        reply_entry(me, &r, &me->tab[slot]);
    }
    reply_close(&r, why);
    timer_arm(me);
}

/**
 * @brief A request to remove an entry of code.
 *
 * @param me The AO.
 * @param r  The request.
 */
static void on_remove(Scheduler * const me, const SchedNameReqEvt *r)
{
    const char *why = flaw_why(r->flaw);
    Reply reply;

    if ( NULL == me->tab )
    {
        why = "memory";
    }
    else if ( NULL != why )
    {
        /* Cut text: refused as it is. */
    }
    else if ( strcmp(r->name, KODE_SCHED_MQTT_NAME) == 0 )
    {
        why = "owner";
    }
    else
    {
        uint8_t const slot = slot_find(me, r->name);

        why = "unknown";
        if ( SCHED_SLOT_NONE != slot )
        {
            if ( (uint8_t)SCHED_BY_CODE == me->tab[slot].owner )
            {
                entry_clear(&me->tab[slot]);
                why = NULL;
            }
            else
            {
                why = "owner";
            }
        }
    }
    reply_open(&reply, r->who);
    reply_close(&reply, why);
    timer_arm(me);
}

/**
 * @brief A request to list one entry or all.
 *
 * @param me The AO.
 * @param r  The request.
 */
static void on_list(Scheduler * const me, const SchedNameReqEvt *r)
{
    const char *why = flaw_why(r->flaw);
    Reply reply;

    reply_open(&reply, r->who);
    if ( NULL == me->tab )
    {
        why = "memory";
    }
    else if ( NULL != why )
    {
        /* Cut text: refused as it is. */
    }
    else if ( r->has_name )
    {
        uint8_t slot = slot_find(me, r->name);

        /* `mqtt` is always in the table; switched off, it says so. */
        if ( strcmp(r->name, KODE_SCHED_MQTT_NAME) == 0 )
        {
            slot = SCHED_SLOT_MQTT;
        }
        why = "unknown";
        if ( SCHED_SLOT_NONE != slot )
        {
            reply_entry(me, &reply, &me->tab[slot]);
            why = NULL;
        }
    }
    else
    {
        for ( uint8_t i = 0U; KODE_SCHED_MAX > i; i++ )
        {
            if ( me->tab[i].used || (SCHED_SLOT_MQTT == i) )
            {
                reply_entry(me, &reply, &me->tab[i]);
            }
        }
    }
    reply_close(&reply, why);
}

#if CONFIG_KODE_LOWPOWER
/**
 * @brief LOWPOWER_ROUND_SIG: give AO_LowPower the earliest time, and rest.
 *
 * Busy while the scheduler holds a minute's work (work_held()). For ASLEEP the
 * wake time is the minute the timer stands armed for (read again from the
 * clock first), in khal_time_us(); the timer is let go, because the tick
 * stops. For OFF there is nothing to wake: the board boots.
 *
 * @param me The AO.
 * @param r  The round.
 */
static void sched_lp_round(Scheduler * const me, const LowPowerRoundEvt * const r)
{
    const char *why = NULL;

    if ( work_held(me) )
    {
        why = SCHED_WHY_WORK;
    }
    else if ( (uint8_t)KODE_LP_OFF != r->target )
    {
        timer_arm_for(me, true);
        if ( SCHED_MINUTE_NONE != me->armed_min )
        {
            uint64_t const due_us = khal_time_at_wall_s(me->armed_min * SCHED_CRON_SEC_MIN);

            if ( !lowpower_wake_at(me->lp_who, due_us) )
            {
                why = SCHED_WHY_WAKE;
            }
        }
    }
    else
    {
        /* OFF: nothing to wake. */
    }
    if ( NULL != why )
    {
        lowpower_ready(me->lp_who, r->round, true, why);
    }
    else
    {
        (void)QTimeEvt_disarm(&me->due);
        me->lp_rest = true;
        lowpower_ready(me->lp_who, r->round, false, NULL);
    }
}

/**
 * @brief LOWPOWER_STATE_SIG: on AWAKE, read the clock again and arm the timer.
 *
 * It repeats (an abandoned round publishes it again), so what is done here is
 * idempotent; only a change of state (prev not AWAKE) begins a wake, and says
 * whether a timer woke the board.
 *
 * @param me The AO.
 * @param s  The change.
 */
static void sched_lp_state(Scheduler * const me, const LowPowerStateEvt * const s)
{
    if ( (uint8_t)KODE_LP_AWAKE == s->state )
    {
        me->lp_rest = false;
        if ( (uint8_t)KODE_LP_AWAKE != s->prev )
        {
            me->timer_wake = ((uint8_t)KODE_LP_CAUSE_TIMER == s->cause);
            me->rest_asked = false;
        }
        timer_arm(me);
    }
}
#endif /* CONFIG_KODE_LOWPOWER */

/**
 * @brief The state every entry is handled in.
 *
 * @param me The AO.
 * @param e  The event.
 * @return The QP status.
 */
static QState Scheduler_on(Scheduler * const me, QEvt const * const e)
{
    QState status;

    switch ( e->sig )
    {
        case SCHED_SET_REQ_SIG: {
            const SchedSetReqEvt * const r = Q_EVT_CAST(SchedSetReqEvt);
            uint8_t slot = SCHED_SLOT_NONE;
            const char *why = "memory";

            if ( NULL != me->tab )
            {
                why = console_set(me, r, &slot);
            }
            set_answer(me, r->who, why, slot);
            status = Q_HANDLED();
            break;
        }
        case SCHED_SET_AO_REQ_SIG: {
            const SchedSetAoReqEvt * const r = Q_EVT_CAST(SchedSetAoReqEvt);
            uint8_t slot = SCHED_SLOT_NONE;
            const char *why = "memory";

            if ( NULL != me->tab )
            {
                why = code_set(me, r, &slot);
            }
            set_answer(me, r->who, why, slot);
            status = Q_HANDLED();
            break;
        }
        case SCHED_REMOVE_REQ_SIG: {
            on_remove(me, Q_EVT_CAST(SchedNameReqEvt));
            status = Q_HANDLED();
            break;
        }
        case SCHED_LIST_REQ_SIG: {
            on_list(me, Q_EVT_CAST(SchedNameReqEvt));
            status = Q_HANDLED();
            break;
        }
        case SCHED_DUE_SIG: {
            if ( me->lp_rest )
            {
                /* It fell due as the board went to sleep: nothing runs now.
                 * The wake time stands, and the AWAKE it brings arms it again. */
                status = Q_HANDLED();
            }
            else if ( timer_due(me) )
            {
                status = Q_TRAN(&Scheduler_waiting);
            }
            else
            {
                status = Q_HANDLED();
            }
            break;
        }
        case SCHED_LINE_DONE_SIG:
        case SCHED_LINE_BUSY_SIG: {
            /* An answer, or a refusal, for a line the bound gave up on, or
             * for none: it belongs to no line, and nothing runs for it. */
            status = Q_HANDLED();
            break;
        }
        case SCHED_RETRY_SIG: {
            /* The console was busy: give the line again. Past the bound the
             * line is already dropped, and there is nothing to give. */
            if ( (!me->waiting_line) && (!me->lp_rest) && line_send(me) )
            {
                status = Q_TRAN(&Scheduler_waiting);
            }
            else
            {
                work_settle(me);
                status = Q_HANDLED();
            }
            break;
        }
        case SCHED_WORK_TMO_SIG: {
            if ( !me->work_on )
            {
                /* An expiry of a bound the work's end already stopped. */
                status = Q_HANDLED();
            }
            else if ( me->waiting_line )
            {
                work_bound(me);
                status = Q_TRAN(&Scheduler_on);
            }
            else
            {
                work_bound(me);
                status = Q_HANDLED();
            }
            break;
        }
        case SCHED_UP_SIG: {
            /* The other AOs have queues now: the timer may run, and AO_Net
             * is handed its first time. */
            me->up = true;
            timer_arm(me);
            status = Q_HANDLED();
            break;
        }
        case SCHED_WALL_SET_SIG: {
            on_wall_set(me);
            status = Q_HANDLED();
            break;
        }
        case SCHED_CHECK_DONE_SIG: {
            /* AO_Net made the check and forgot its time: it is owed the next. */
            KLOG_I(TAG, "the mqtt check was made");
            me->chk_sent = SCHED_CHECK_UNSENT;
            chk_sync(me, true);
            status = Q_HANDLED();
            break;
        }
#if CONFIG_KODE_LOWPOWER
        case LOWPOWER_ROUND_SIG: {
            sched_lp_round(me, Q_EVT_CAST(LowPowerRoundEvt));
            status = Q_HANDLED();
            break;
        }
        case LOWPOWER_STATE_SIG: {
            sched_lp_state(me, Q_EVT_CAST(LowPowerStateEvt));
            status = Q_HANDLED();
            break;
        }
        case LOWPOWER_REFUSED_SIG: {
            const LowPowerRefusedEvt * const f = Q_EVT_CAST(LowPowerRefusedEvt);

            if ( (uint8_t)KODE_LOWPOWER_SRC_SCHED == f->src )
            {
                KLOG_I(TAG, "the board stays awake: %s", f->why);
                /* A refusal that passes by itself soon lets the next pass ask
                 * again. Any other (the board in use, a cable in) stands for
                 * the wake: asking every minute would meet the same answer. */
                if ( rest_passes(f->why) )
                {
                    me->rest_asked = false;
                }
            }
            status = Q_HANDLED();
            break;
        }
#endif
        default: {
            status = Q_SUPER(&QHsm_top);
            break;
        }
    }
    return status;
}

/**
 * @brief A line is out, and the console has not answered it.
 *
 * @param me The AO.
 * @param e  The event.
 * @return The QP status.
 */
static QState Scheduler_waiting(Scheduler * const me, QEvt const * const e)
{
    QState status;

    switch ( e->sig )
    {
        case Q_ENTRY_SIG: {
            me->waiting_line = true;
            status = Q_HANDLED();
            break;
        }
        case Q_EXIT_SIG: {
            me->waiting_line = false;
            status = Q_HANDLED();
            break;
        }
        case SCHED_LINE_DONE_SIG: {
            me->out_slot     = SCHED_SLOT_NONE;
            me->waiting_line = false;
            if ( me->out_ends && me->lp_rest )
            {
                /* The line ended the wake (`sleep`, `power_off`) and the board
                 * rests: the lines behind it are dropped. Behind any other
                 * line they hold every round (work_held()). */
                lines_drop(me, "the board rests before its turn");
            }
            me->out_ends = false;
            if ( line_send(me) )
            {
                status = Q_TRAN(&Scheduler_waiting);
            }
            else
            {
                work_settle(me);
                status = Q_TRAN(&Scheduler_on);
            }
            break;
        }
        case SCHED_LINE_BUSY_SIG: {
            /* The console took nothing: the line goes again after
             * KODE_SCHED_RETRY_MS, until the bound ends the tries
             * (work_bound()). An entry removed meanwhile has no slot. */
            if ( SCHED_SLOT_NONE != me->out_slot )
            {
                SchedEntry * const en = &me->tab[me->out_slot];

                en->pending = true;
                KLOG_D(TAG, "the console is busy; the entry '%s' is tried again in %u ms",
                       en->name, (unsigned)KODE_SCHED_RETRY_MS);
            }
            me->out_slot     = SCHED_SLOT_NONE;
            me->out_ends     = false;
            me->waiting_line = false;
            /* Disarmed first, as work_arm() does: QP refuses to arm a time
             * event that is armed. */
            (void)QTimeEvt_disarm(&me->retry);
            QTimeEvt_armX(&me->retry, KQP_TICKS_PER_MS(KODE_SCHED_RETRY_MS), 0U);
            status = Q_TRAN(&Scheduler_on);
            break;
        }
        default: {
            status = Q_SUPER(&Scheduler_on);
            break;
        }
    }
    return status;
}

/**
 * @brief The hook kdrv_clocks calls after the wall clock was written.
 *
 * It runs in the thread that wrote the clock (the console's, the time
 * server's, the Time page's), so it posts a static event and does no more.
 */
static void sched_wall_written(void)
{
    static const QEvt written = QEVT_INITIALIZER(SCHED_WALL_SET_SIG);

    if ( !QACTIVE_POST_X(AO_Scheduler, &written, 0U, NULL) )
    {
        KLOG_W(TAG, "the clock was set and the scheduler was not told: the queue is full");
    }
}

/**
 * @brief Read the store, arm the timer, and start in `on`.
 *
 * @param me  The AO.
 * @param par Unused.
 * @return The transition to `on`.
 */
static QState Scheduler_initial(Scheduler * const me, void const * const par)
{
    (void)par;

    if ( NULL != me->tab )
    {
        store_load(me);
    }
#if CONFIG_KODE_LOWPOWER
    me->lp_who = lowpower_join(&me->super, "scheduler");
    if ( KODE_LOWPOWER_WHO_NONE != me->lp_who )
    {
        QActive_subscribe(&me->super, LOWPOWER_ROUND_SIG);
        QActive_subscribe(&me->super, LOWPOWER_STATE_SIG);
        QActive_subscribe(&me->super, LOWPOWER_REFUSED_SIG);
    }
#endif
    kdrv_clocks_wall_on_set(&sched_wall_written);
    timer_arm(me);
    KLOG_I(TAG, "up");
    return Q_TRAN(&Scheduler_on);
}

/* ---- Construction and the doors --------------------------------------------------- */

void Scheduler_ctor(void)
{
    /* The table is taken once and kept across a reconstruction, and the time
     * events are constructed once: the host suite rebuilds the AO per case. */
    static SchedEntry *table;
    static bool built;
    Scheduler * const me = &l_sched;
    sched_cron_t cron;

    QActive_ctor(&me->super, Q_STATE_CAST(&Scheduler_initial));
    if ( built )
    {
        /* Constructed again: a time event that is still linked into QF's
         * list must not be constructed over, only stopped. */
        (void)QTimeEvt_disarm(&me->due);
        (void)QTimeEvt_disarm(&me->work);
        (void)QTimeEvt_disarm(&me->retry);
    }
    else
    {
        QTimeEvt_ctorX(&me->due, &me->super, SCHED_DUE_SIG, 0U);
        QTimeEvt_ctorX(&me->work, &me->super, SCHED_WORK_TMO_SIG, 0U);
        QTimeEvt_ctorX(&me->retry, &me->super, SCHED_RETRY_SIG, 0U);
        built = true;
    }
    me->last_min     = SCHED_MINUTE_UNSET;
    me->armed_min    = SCHED_MINUTE_NONE;
    me->chk_sent     = SCHED_CHECK_UNSENT;
    me->out_slot     = SCHED_SLOT_NONE;
    me->out_ends     = false;
    me->up           = false;
    me->lp_who       = KODE_LOWPOWER_WHO_NONE;
    me->waiting_line = false;
    me->work_on      = false;
    me->lp_rest      = false;
    me->timer_wake   = false;
    me->rest_asked   = false;
    me->chk_owed     = false;

    if ( NULL == table )
    {
        /* cppcheck-suppress misra-c2012-11.5 ; khal_alloc_psram() answers void *: PSRAM is taken by name (rule 8) */
        table = (SchedEntry *)khal_alloc_psram(SCHED_TABLE_ALIGN,
                                                 KODE_SCHED_MAX * sizeof(SchedEntry));
    }
    me->tab = table;
    if ( NULL == me->tab )
    {
        KLOG_E(TAG, "no memory for the table; the scheduler runs nothing");
    }
    else
    {
        (void)memset(me->tab, 0, KODE_SCHED_MAX * sizeof(SchedEntry));
        for ( uint8_t i = 0U; KODE_SCHED_MAX > i; i++ )
        {
            entry_clear(&me->tab[i]);
        }
        /* `mqtt` is on at its default until the store says otherwise. */
        if ( sched_cron_parse(KODE_SCHED_MQTT_DEFAULT, &cron, NULL) == KODE_OK )
        {
            entry_fill(&me->tab[SCHED_SLOT_MQTT], SCHED_BY_BUILTIN, KODE_SCHED_MQTT_NAME,
                       KODE_SCHED_MQTT_DEFAULT, &cron, NULL);
        }
    }
}

/* A small stack: a step is one pass over sixteen entries and a post, and the
 * table is in PSRAM. The queue holds the requests of an app that makes all its
 * entries at its boot, and the answer of the line in hand. */
/* cppcheck-suppress misra-c2012-8.9 ; AO_STORAGE()'s file-scope storage, the shape every AO has */
AO_STORAGE(scheduler, KODE_SCHED_QUEUE_LEN, KQP_AO_STACK_SMALL);

void kaos_scheduler_register(void)
{
#if CONFIG_KODEOS_CONSOLE
    kaos_require(kverbs_add(&scheduler_verbs), "scheduler verbs");
#endif
}

void kaos_scheduler_start(void)
{
    Scheduler_ctor();
    kaos_scheduler_register();
    AO_START(AO_Scheduler, scheduler, KODE_PRIO_SCHEDULER);
}

/**
 * @brief Allocate a request, or log that none was left.
 *
 * @param what Words for the log.
 * @return false: nothing was left (callers return it).
 */
static bool request_dropped(const char *what)
{
    KLOG_W(TAG, "out of events; the request to %s was dropped", what);
    return false;
}

/**
 * @brief Post a request to the AO with a margin.
 *
 * @param e    The request, freshly allocated.
 * @param what Words for the log.
 * @return true when it was posted.
 */
static bool request_post(const QEvt * const e, const char *what)
{
    bool posted = QACTIVE_POST_X(AO_Scheduler, e, KODE_SCHED_REQ_MARGIN, NULL);

    if ( !posted )
    {
        KLOG_W(TAG, "the request to %s was refused: the queue is full", what);
    }
    return posted;
}

bool sched_request_set(QActive *who, const char *name, const char *when,
                       const char *cmd)
{
    SchedSetReqEvt * const e = Q_NEW_X(SchedSetReqEvt, 0U, SCHED_SET_REQ_SIG);
    bool posted = false;

    if ( NULL == e )
    {
        posted = request_dropped("set an entry");
    }
    else
    {
        e->who  = who;
        e->flaw = (uint8_t)SCHED_FLAW_NONE;
        if ( (NULL == name) || !copy_text(e->name, sizeof(e->name), name) )
        {
            e->flaw = (uint8_t)SCHED_FLAW_NAME;
        }
        else if ( (NULL == when) || !copy_text(e->when, sizeof(e->when), when) )
        {
            e->flaw = (uint8_t)SCHED_FLAW_WHEN;
        }
        else if ( !copy_text(e->cmd, sizeof(e->cmd), cmd) )
        {
            e->flaw = (uint8_t)SCHED_FLAW_CMD;
        }
        else
        {
            /* Every text fit. */
        }
        posted = request_post(&e->super, "set an entry");
    }
    return posted;
}

/* cppcheck-suppress misra-c2012-8.7 ; the console verb and the facade, later changes, are its callers; the host suite calls it already; remove with the first */
bool sched_request_set_ao(QActive *who, const char *name, const char *when,
                          QActive *target, enum_t sig)
{
    SchedSetAoReqEvt * const e = Q_NEW_X(SchedSetAoReqEvt, 0U, SCHED_SET_AO_REQ_SIG);
    bool posted = false;

    if ( NULL == e )
    {
        posted = request_dropped("set an entry of code");
    }
    else
    {
        e->who      = who;
        e->target   = target;
        e->fire_sig = sig;
        e->flaw     = (uint8_t)SCHED_FLAW_NONE;
        if ( (NULL == name) || !copy_text(e->name, sizeof(e->name), name) )
        {
            e->flaw = (uint8_t)SCHED_FLAW_NAME;
        }
        else if ( (NULL == when) || !copy_text(e->when, sizeof(e->when), when) )
        {
            e->flaw = (uint8_t)SCHED_FLAW_WHEN;
        }
        else
        {
            /* Every text fit. */
        }
        posted = request_post(&e->super, "set an entry of code");
    }
    return posted;
}

/**
 * @brief Post a remove or a list request.
 *
 * @param sig   SCHED_REMOVE_REQ_SIG or SCHED_LIST_REQ_SIG.
 * @param who   Who is answered.
 * @param name  The entry; NULL for every one (a list).
 * @param what  Words for the log.
 * @return true when it was posted.
 */
static bool name_request(enum_t sig, QActive *who, const char *name, const char *what)
{
    SchedNameReqEvt * const e = Q_NEW_X(SchedNameReqEvt, 0U, sig);
    bool posted = false;

    if ( NULL == e )
    {
        posted = request_dropped(what);
    }
    else
    {
        e->who      = who;
        e->has_name = (NULL != name);
        e->flaw     = (uint8_t)SCHED_FLAW_NONE;
        e->name[0]  = '\0';
        if ( (NULL != name) && !copy_text(e->name, sizeof(e->name), name) )
        {
            e->flaw = (uint8_t)SCHED_FLAW_NAME;
        }
        posted = request_post(&e->super, what);
    }
    return posted;
}

/* cppcheck-suppress misra-c2012-8.7 ; the console verb and the facade, later changes, are its callers; the host suite calls it already; remove with the first */
bool sched_request_remove(QActive *who, const char *name)
{
    bool posted = false;

    if ( NULL != name )
    {
        posted = name_request(SCHED_REMOVE_REQ_SIG, who, name, "remove an entry");
    }
    return posted;
}

bool sched_request_list(QActive *who, const char *name)
{
    bool posted = false;

    if ( NULL != who )
    {
        posted = name_request(SCHED_LIST_REQ_SIG, who, name, "list the entries");
    }
    return posted;
}

void sched_line_done(void)
{
    static const QEvt done = QEVT_INITIALIZER(SCHED_LINE_DONE_SIG);

    /* Margin 0: every request leaves KODE_SCHED_REQ_MARGIN slots free for this.
     * A refusal can only be logged. */
    if ( !QACTIVE_POST_X(AO_Scheduler, &done, 0U, NULL) )
    {
        KLOG_E(TAG, "the console's answer to a line was dropped: the queue is full");
    }
}

void sched_line_busy(void)
{
    static const QEvt busy = QEVT_INITIALIZER(SCHED_LINE_BUSY_SIG);

    /* Margin 0, like sched_line_done(); if refused, the work's bound frees
     * the line. */
    if ( !QACTIVE_POST_X(AO_Scheduler, &busy, 0U, NULL) )
    {
        KLOG_E(TAG, "the console's refusal of a line was dropped: the queue is full");
    }
}

void sched_system_up(void)
{
    static const QEvt up = QEVT_INITIALIZER(SCHED_UP_SIG);

    /* Static, so no pool can refuse it; the queue holds no more than what
     * the AOs started before the call have asked, far short of its length. */
    if ( !QACTIVE_POST_X(AO_Scheduler, &up, 0U, NULL) )
    {
        KLOG_E(TAG, "the system's start was not told: the queue is full, and nothing will fire");
    }
}

void sched_check_done(int64_t passed_s)
{
    SchedCheckDoneEvt * const e = Q_NEW_X(SchedCheckDoneEvt, 0U, SCHED_CHECK_DONE_SIG);

    if ( NULL == e )
    {
        KLOG_W(TAG, "out of events; the mqtt check made was not told (the next time goes out once its hold ends)");
    }
    else
    {
        e->passed_s = passed_s;
        /* A refused post recycles the event itself. */
        if ( !QACTIVE_POST_X(AO_Scheduler, &e->super, 0U, NULL) )
        {
            KLOG_W(TAG, "the mqtt check made was not told: the queue is full (the next time goes out once its hold ends)");
        }
    }
}
