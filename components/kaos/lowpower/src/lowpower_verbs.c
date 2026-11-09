/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file lowpower_verbs.c
 * @brief AO_LowPower's console verbs: `sleep`, `power_off` and `lowpower_info`, registered by kaos_lowpower_start().
 */
#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"

#include "kaos.h"
#include "kdrv_radio.h"
#include "khal_power.h"
#include "khal_system.h"
#include "kode_log.h"
#include "kqp.h"

#include "console_verbs.h"
#include "kaos_wire.h"
#include "kverbs.h"

/* AO_Radio, named weakly (KAOS_WEAK, kaos.h): `lowpower_info` reads its last
 * wake note when the app linked it, and must not link it itself. */
KAOS_WEAK void kaos_radio_start(void);
KAOS_WEAK bool radio_sleep_last_note(RadioSleepNoteEvt *out);

/** @brief The deadline of `sleep` and `power_off`: AO_LowPower begins a refused
 *         request again for KODE_LOWPOWER_REQUEST_RETRY_WINDOW_MS, a last round
 *         can still run KODE_LOWPOWER_ROUND_MS after that, and the default
 *         deadline is the margin on top. */
#define LP_V_TIMEOUT_MS  (KODE_LOWPOWER_REQUEST_RETRY_WINDOW_MS \
                          + KODE_LOWPOWER_ROUND_MS + (uint32_t)CONSOLE_CMD_TIMEOUT_MS_N)

/** @brief Records of the summary one slice emits: the console's own slice. */
#define LP_V_SLICE  KAOS_WIRE_SLICE

/** @brief Percent, for the share of time spent in light sleep. */
#define LP_V_PERCENT  100ULL

/** @brief Which of this AO's verbs holds the console. */
typedef enum {
    LP_V_NONE = 0,   /**< None of them.                                       */
    LP_V_STATE,      /**< sleep and power_off: closed by LOWPOWER_STATE_SIG, or by
                      *   LOWPOWER_REFUSED_SIG carrying this command's source. */
    LP_V_LAST        /**< lowpower_info last=on: the last rest, in paced slices. */
} lp_verb_kind_t;

static lp_verb_kind_t s_lp_kind;   /**< The verb in flight, LP_V_NONE between. */
static uint16_t       s_lp_next;   /**< LP_V_LAST: the next record to emit.   */

/**
 * @brief Ask AO_LowPower for a state and wait for its answer.
 *
 * An accepted request is answered by LOWPOWER_STATE_SIG, a refused one by
 * LOWPOWER_REFUSED_SIG once AO_LowPower's retry window is over (LP_V_TIMEOUT_MS
 * covers it). The watchdog answers an event that was never published.
 *
 * @param c      The verb.
 * @param target The state wanted.
 */
static void lowpower_ask(const kverb_t *c, kode_lp_state_t target)
{
    console_begin(c->verb);
    console_extend(LP_V_TIMEOUT_MS);
    s_lp_kind = LP_V_STATE;
    lowpower_request(target, console_src());
}

/** @brief sleep: take the board to ASLEEP, light sleep with the radios up. */
static void h_sleep(const kverb_t *c, const karg_val_t *v)
{
    (void)v;
    lowpower_ask(c, KODE_LP_ASLEEP);
}

/** @brief power_off: take the board to OFF, deep sleep; only BOT wakes it. */
static void h_power_off(const kverb_t *c, const karg_val_t *v)
{
    (void)v;
    lowpower_ask(c, KODE_LP_OFF);
}

/** @brief The share of the time since boot spent in light sleep, 0..100. */
static uint32_t lp_slept_pct(const khal_power_stats_t *st)
{
    uint32_t pct = 0U;

    if ( 0U != st->since_us )
    {
        pct = (uint32_t)((st->slept_us * LP_V_PERCENT) / st->since_us);
    }
    return pct;
}

/* `lowpower_info` is two records, because one does not fit KODE_LOG_LINE_MAX.
 * The first carries the state machine and the last refusal, quoted last into
 * what the numbers leave; the second the counters. Both widest forms are checked
 * here, so a field added to either fails the build instead of cutting a record. */
/** The first record with every number at its widest, every word its longest
 *  and nothing in the value of `last_busy`. */
#define LP_STATE_WIDEST ">|lowpower state=asleep for_s=4294967295 " \
                        "cause=off_after sleep_after=4294967295 " \
                        "off_after=4294967295 wakes_bot=4294967295 " \
                        "wakes_ble=4294967295 wakes_mqtt=4294967295 " \
                        "wakes_timer=4294967295 last_busy="
/** The second record with every counter at its widest. */
#define LP_STATS_WIDEST ">|lowpower_stats slept_pct=100 sleeps=4294967295 " \
                        "wakes_c5=4294967295 wakes_tx=4294967295 " \
                        "tx_sta=4294967295 tx_serial=4294967295 " \
                        "tx_hci=4294967295 knocks_max=65535 deaf=4294967295"
/** The least a quoted refusal needs: its longest text, both quotes and the
 *  NUL. A refusal with an escape in it can need more and is cut by
 *  console_kv_fit(). */
#define LP_BUSY_MIN    (KODE_LOWPOWER_REFUSAL_MAX + 2U)
/** What the first record leaves for `last_busy` at its widest (the line's
 *  newline takes the byte after it, kode_log_reply_src()). */
#define LP_BUSY_ROOM   (KODE_LOG_LINE_MAX - 1U - sizeof(LP_STATE_WIDEST))
_Static_assert(LP_BUSY_ROOM >= LP_BUSY_MIN,
               "lowpower_info: the widest first record leaves a refusal room");
_Static_assert(sizeof(LP_STATS_WIDEST) <= (KODE_LOG_LINE_MAX - 1U),
               "lowpower_info: the widest second record fits one line");

/** The room a counter's decimal text needs: ten digits and the NUL. */
#define LP_COUNT_CHARS 11U

/** Where `lowpower_info`'s one key, `last`, lands in the parsed values. */
#define LP_INFO_ARG_LAST  0U

/** @brief A counter of the wake note for a record: its number, or `-` for none. */
static const char *lp_count(char *dst, size_t cap, bool have, uint32_t value)
{
    const char *out = "-";

    if ( have )
    {
        (void)snprintf(dst, cap, "%lu", (unsigned long)value);
        out = dst;
    }
    return out;
}

/** The summary's header record with every number at its widest. */
#define LP_LAST_WIDEST ">|lowpower_last start=4294967295 for_s=4294967295 " \
                       "slept_pct=100 sleeps=4294967295 wakes_c5=4294967295 " \
                       "wakes_tx=4294967295 deaf=4294967295 end=off_after " \
                       "samples=60"
/** One sample record with every number at its widest. */
#define LP_SAMPLE_WIDEST ">|lp_sample t=4294967295 ma=-32768 mv=65535"
_Static_assert(sizeof(LP_LAST_WIDEST) <= (KODE_LOG_LINE_MAX - 1U),
               "lowpower_info last=on: the widest header record fits one line");
_Static_assert(sizeof(LP_SAMPLE_WIDEST) <= (KODE_LOG_LINE_MAX - 1U),
               "lowpower_info last=on: the widest sample record fits one line");

/** The wire's words for a kode_lp_cause_t, in its order. */
static const char *const LP_CAUSE_WORDS[] = {
    "request", "idle", "off_after", "bot", "ble", "mqtt", "cable", "timer",
    "top", "boot"
};
_Static_assert(Q_DIM(LP_CAUSE_WORDS) == ((size_t)KODE_LP_CAUSE_BOOT + 1U),
               "a word for every cause, BOOT being the last");

/** The word for a summary whose rest left no end cause. */
#define LP_END_OPEN_WORD "asleep"

/** @brief What one slice of the summary's listing came to. */
typedef enum {
    LP_SLICE_MORE,     /**< Records are still owed.                       */
    LP_SLICE_DONE,     /**< The listing is complete.                      */
    LP_SLICE_FAILED    /**< No memory to read the summary into.           */
} lp_slice_t;

/**
 * @brief Emit up to LP_V_SLICE records of the summary, from the cursor.
 *
 * Record 0 is the header, then the samples, oldest first. The summary is read
 * afresh for every slice, so a command the watchdog closes leaves nothing held.
 * Without a summary the header says so and the listing is done.
 *
 * @return What came of the slice.
 */
static lp_slice_t lp_last_slice(void)
{
    /* cppcheck-suppress misra-c2012-11.5 ; khal_alloc_psram() answers void *: PSRAM is taken by name (rule 8) */
    lowpower_last_t * const l = (lowpower_last_t *)khal_alloc_psram(
        sizeof(uint32_t), sizeof(lowpower_last_t));
    lp_slice_t res = LP_SLICE_FAILED;

    if ( NULL != l )
    {
        res = LP_SLICE_DONE;
        if ( !lowpower_last_read(l) )
        {
            console_reply(">|lowpower_last present=no");
        }
        else
        {
            uint16_t const total = (uint16_t)(1U + l->count);
            uint16_t pos = s_lp_next;
            uint16_t sent = 0U;

            while ( (pos < total) && ((uint16_t)LP_V_SLICE > sent) )
            {
                if ( 0U == pos )
                {
                    char start[LP_COUNT_CHARS];
                    const char *end = LP_END_OPEN_WORD;

                    if ( (uint8_t)KODE_LOWPOWER_LAST_OPEN != l->end_cause )
                    {
                        end = console_word(LP_CAUSE_WORDS, Q_DIM(LP_CAUSE_WORDS),
                                           l->end_cause);
                    }
                    console_reply(">|lowpower_last start=%s for_s=%lu slept_pct=%u"
                                  " sleeps=%lu wakes_c5=%lu wakes_tx=%lu deaf=%lu"
                                  " end=%s samples=%u",
                                  lp_count(start, sizeof(start), 0U != l->start_s,
                                           l->start_s),
                                  (unsigned long)l->duration_s,
                                  (unsigned)l->slept_pct, (unsigned long)l->sleeps,
                                  (unsigned long)l->wakes_c5,
                                  (unsigned long)l->wakes_tx,
                                  (unsigned long)l->deaf,
                                  end, (unsigned)l->count);
                }
                else
                {
                    const lowpower_sample_t * const sm =
                        lowpower_last_sample(l, (uint8_t)(pos - 1U));

                    console_reply(">|lp_sample t=%lu ma=%d mv=%u",
                                  (unsigned long)sm->t_s, (int)sm->ma, (unsigned)sm->mv);
                }
                pos += 1U;
                sent += 1U;
            }
            s_lp_next = pos;
            if ( pos < total )
            {
                res = LP_SLICE_MORE;
            }
        }
        khal_free(l);
    }
    return res;
}

/** @brief Emit the next slice of the summary, then close the command or pace the next. */
static void lp_last_advance(void)
{
    lp_slice_t const res = lp_last_slice();

    if ( LP_SLICE_MORE == res )
    {
        console_pace();
    }
    else
    {
        if ( LP_SLICE_DONE == res )
        {
            console_reply(">ok");
        }
        else
        {
            console_reply_err(KCLI_ERR_REJECT, "memory");
        }
        s_lp_kind = LP_V_NONE;
        console_end();
    }
}

/**
 * @brief lowpower_info: the board's rest, two records.
 *
 * `>|lowpower`: state, for_s, cause, the two stored times in seconds (0 is never),
 * the wakes, and last_busy last, as the one field that can run long.
 * `>|lowpower_stats`: light-sleep share and count, the C5's wakes, frames queued
 * while it slept (`-` until a wake is noted), most knocks per wake, deaf wakes.
 * `last=on` prints the last rest instead: a `>|lowpower_last` header and one
 * `>|lp_sample` per gauge sample, oldest first, in slices; `present=no` if none.
 */
static void h_lowpower_info(const kverb_t *c, const karg_val_t *v)
{
    /* The wire's words, in kode_lp_state_t order. */
    static const char *const STATE_WORDS[] = { "awake", "asleep", "off" };
    lowpower_info_t          lp;
    khal_power_stats_t       st;
    kdrv_radio_sleep_stats_t rs;
    RadioSleepNoteEvt        note;
    char                     busy[LP_BUSY_ROOM];
    char                     tx_sta[LP_COUNT_CHARS];
    char                     tx_serial[LP_COUNT_CHARS];
    char                     tx_hci[LP_COUNT_CHARS];
    const char              *refusal;
    const char              *last_busy = "-";
    bool                     have;

    if ( v[LP_INFO_ARG_LAST].present && v[LP_INFO_ARG_LAST].b )
    {
        s_lp_next = 0U;
        console_begin(c->verb);
        s_lp_kind = LP_V_LAST;
        lp_last_advance();
        return;
    }
    lowpower_info_get(&lp);
    khal_power_stats(&st);
    kdrv_radio_sleep_stats(&rs);
    (void)memset(&note, 0, sizeof(note));
    have = false;
    if ( kaos_linked(&kaos_radio_start) )
    {
        have = radio_sleep_last_note(&note);
    }
    refusal = lowpower_last_refusal();
    if ( (NULL != refusal) && ('\0' != refusal[0]) )
    {
        last_busy = console_kv_fit(busy, sizeof(busy), refusal, "last_busy");
    }

    console_reply(">|lowpower state=%s for_s=%lu cause=%s sleep_after=%lu"
                  " off_after=%lu wakes_bot=%lu wakes_ble=%lu wakes_mqtt=%lu"
                  " wakes_timer=%lu last_busy=%s",
                  console_word(STATE_WORDS, Q_DIM(STATE_WORDS), lp.state),
                  (unsigned long)lp.for_s,
                  console_word(LP_CAUSE_WORDS, Q_DIM(LP_CAUSE_WORDS), lp.cause),
                  (unsigned long)lp.sleep_after_s, (unsigned long)lp.off_after_s,
                  (unsigned long)lp.wakes_bot, (unsigned long)lp.wakes_ble,
                  (unsigned long)lp.wakes_mqtt, (unsigned long)lp.wakes_timer,
                  last_busy);
    console_reply(">|lowpower_stats slept_pct=%lu sleeps=%lu wakes_c5=%lu"
                  " wakes_tx=%lu tx_sta=%s tx_serial=%s tx_hci=%s knocks_max=%u"
                  " deaf=%lu",
                  (unsigned long)lp_slept_pct(&st), (unsigned long)st.sleeps,
                  (unsigned long)rs.wakes_c5, (unsigned long)rs.wakes_tx,
                  lp_count(tx_sta, sizeof(tx_sta), have, note.tx_sta),
                  lp_count(tx_serial, sizeof(tx_serial), have, note.tx_serial),
                  lp_count(tx_hci, sizeof(tx_hci), have, note.tx_hci),
                  (unsigned)rs.knocks_max, (unsigned long)rs.deaf);
    console_reply(">ok");
}

/**
 * @brief The events of this AO's verbs while one holds the console.
 *
 * ASLEEP or OFF answers `sleep` and `power_off`; AWAKE and RADIO do not (a wake
 * or a round given up, which the refusal explains). A refusal answers only the
 * port that asked. The pacer's tick carries `lowpower_info last=on` to its next
 * slice, or waits out a congested port.
 *
 * @param e The event.
 * @return true when it was this AO's verb's.
 */
static bool lowpower_answer(QEvt const *e)
{
    bool handled = false;

    if ( (LP_V_STATE == s_lp_kind) && ((QSignal)LOWPOWER_STATE_SIG == e->sig) )
    {
        uint8_t const state = Q_EVT_CAST(LowPowerStateEvt)->state;

        if ( ((uint8_t)KODE_LP_ASLEEP == state) || ((uint8_t)KODE_LP_OFF == state) )
        {
            console_reply(">ok");
            s_lp_kind = LP_V_NONE;
            console_end();
        }
        handled = true;
    }
    else if ( (LP_V_STATE == s_lp_kind) && ((QSignal)LOWPOWER_REFUSED_SIG == e->sig) )
    {
        LowPowerRefusedEvt const * const r = Q_EVT_CAST(LowPowerRefusedEvt);

        if ( console_src() == r->src )
        {
            char quoted[CONSOLE_QUOTED_MAX(KODE_LOWPOWER_REFUSAL_MAX)];

            console_reply(">err code=%u msg=%s", (unsigned)KCLI_ERR_BUSY,
                          console_kv_text(quoted, sizeof(quoted), r->why));
            s_lp_kind = LP_V_NONE;
            console_end();
        }
        handled = true;
    }
    else if ( (LP_V_LAST == s_lp_kind) && ((QSignal)CONSOLE_SLICE_SIG == e->sig) )
    {
        if ( console_congested() )
        {
            console_pace();
        }
        else
        {
            lp_last_advance();
        }
        handled = true;
    }
    else
    {
        /* Not this AO's verb's. */
    }
    return handled;
}

/**
 * @brief The watchdog fired on one of this AO's verbs: forget it, and let the
 *        console answer the timeout.
 *
 * @return false: the console closes the command.
 */
static bool lowpower_timeout(void)
{
    s_lp_kind = LP_V_NONE;
    return false;
}

/* `lowpower_info [last=on]`: the summary of the last rest instead of the state. */
static const karg_spec_t A_LOWPOWER_INFO[] = {
    { .key = "last", .required = false, .type = KARG_BOOL },
};

static const kverb_t LOWPOWER_VERBS[] = {
    { "sleep",         NULL,            0U,                          h_sleep         },
    { "power_off",     NULL,            0U,                          h_power_off     },
    { "lowpower_info", A_LOWPOWER_INFO, KVERB_KEYS(A_LOWPOWER_INFO), h_lowpower_info },
};

const kverbs_t lowpower_verbs = {
    .verbs   = LOWPOWER_VERBS,
    .n       = sizeof(LOWPOWER_VERBS) / sizeof(LOWPOWER_VERBS[0]),
    .answer  = lowpower_answer,
    .timeout = lowpower_timeout,
};
