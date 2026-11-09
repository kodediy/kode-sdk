/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file scheduler_verbs.c
 * @brief AO_Scheduler's console verb: `cron`, registered by kaos_scheduler_start().
 */
#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"

#include "kaos.h"
#include "kode_log.h"
#include "kqp.h"

#include "console_grammar.h"
#include "console_verbs.h"
#include "kverbs.h"

#define TAG "kcli"

/** @brief The longest `next=`: a 64-bit seconds count with its sign, and the NUL. */
#define CRON_NEXT_CHARS    21U
/** @brief The least of an entry's line a `>|cron` record still shows when its other fields are at their widest. */
#define CRON_CMD_ROOM_MIN  32U
/** @brief The widest `>|cron` record but for the values of name, when, next and cmd (their quotes counted). */
#define CRON_FIXED_WIDEST  ">|cron name=\"\" when=\"\" cmd= owner=console next= stored=yes"
_Static_assert((KODE_LOG_LINE_MAX - 1U)
               >= (sizeof(CRON_FIXED_WIDEST) + KODE_SCHED_NAME_MAX
                   + KODE_SCHED_WHEN_MAX + CRON_NEXT_CHARS + CRON_CMD_ROOM_MIN),
               "cron: the widest record leaves the line room on one line");

/* Where `cron`'s keys land in the parsed values. */
#define CRON_ARG_NAME  0U   /**< `name=`. */
#define CRON_ARG_WHEN  1U   /**< `when=`. */
#define CRON_ARG_CMD   2U   /**< `cmd=`.  */

/** @brief `cron` holds the console: SCHED_ENTRY_SIG and SCHED_END_SIG are its answer. */
static bool s_cron_asked;

/** @brief How the console says what AO_Scheduler refused for. */
typedef struct {
    const char *why;    /**< SchedEndEvt::why.                                 */
    kcli_err_t  code;   /**< The `code=` of the refusal.                        */
    const char *msg;    /**< Its `msg=`: one word.                              */
    bool        note;   /**< The reason also goes to the log, as a NOTICE.      */
} cron_refusal_t;

/**
 * @brief End `cron` with the refusal that matches AO_Scheduler's reason.
 *
 * A reason about the expression also reaches the log, so the reply stays one
 * word and the log says which field.
 *
 * @param why SchedEndEvt::why.
 */
static void cron_refuse(const char *why)
{
    /* Every reason AO_Scheduler gives. Any other is the verb's own failure,
     * `msg=cron`. */
    static const cron_refusal_t refusals[] = {
        { "full",    KCLI_ERR_BUSY,   "full",    false },
        { "owner",   KCLI_ERR_REJECT, "owner",   false },
        { "when",    KCLI_ERR_VALUE,  "when",    true  },
        { "fields",  KCLI_ERR_VALUE,  "when",    true  },
        { "range",   KCLI_ERR_VALUE,  "when",    true  },
        { "step",    KCLI_ERR_VALUE,  "when",    true  },
        { "syntax",  KCLI_ERR_VALUE,  "when",    true  },
        { "never",   KCLI_ERR_VALUE,  "when",    true  },
        { "name",    KCLI_ERR_VALUE,  "name",    false },
        { "unknown", KCLI_ERR_VALUE,  "name",    false },
        { "cmd",     KCLI_ERR_VALUE,  "cmd",     false },
        { "partial", KCLI_ERR_REJECT, "partial", false },
        { "memory",  KCLI_ERR_REJECT, "memory",  false },
    };
    kcli_err_t  code = KCLI_ERR_REJECT;
    const char *msg  = "cron";

    for ( size_t i = 0U; (sizeof(refusals) / sizeof(refusals[0])) > i; ++i )
    {
        if ( strcmp(why, refusals[i].why) == 0 )
        {
            code = refusals[i].code;
            msg  = refusals[i].msg;
            if ( refusals[i].note )
            {
                KLOG_N(TAG, "cron: the expression is refused: %s", why);
            }
        }
    }
    console_reply_err(code, msg);
}

/**
 * @brief One `>|cron` record: an entry as AO_Scheduler holds it.
 *
 * The line's value is the one text the board does not choose, so it is quoted
 * last into what the other fields leave and cut, with a warning, when it does
 * not fit (console_kv_fit()).
 *
 * @param s The entry.
 */
static void cron_record(const SchedEntryEvt *s)
{
    /* The word of each sched_owner_t. */
    static const char *const owners[] = { "console", "code", "builtin" };
    char qn[CONSOLE_QUOTED_MAX(KODE_SCHED_NAME_MAX)];
    char qw[CONSOLE_QUOTED_MAX(KODE_SCHED_WHEN_MAX)];
    char qc[KODE_LOG_LINE_MAX];
    char head[sizeof(">|cron name= when= cmd=") + sizeof(qn) + sizeof(qw)];
    char tail[sizeof(" owner=builtin next= stored=yes") + CRON_NEXT_CHARS];
    char digits[CRON_NEXT_CHARS];
    const char *owner = "?";
    const char *next = "-";
    const char *stored = "no";
    const char *cmd = "-";
    size_t fixed;
    size_t room = CRON_CMD_ROOM_MIN;

    _Static_assert((sizeof(owners) / sizeof(owners[0])) == ((size_t)SCHED_BY_BUILTIN + 1U),
                   "one word per sched_owner_t");
    if ( (sizeof(owners) / sizeof(owners[0])) > s->owner )
    {
        owner = owners[s->owner];
    }
    if ( 0 <= s->next_s )
    {
        (void)snprintf(digits, sizeof(digits), "%lld", (long long)s->next_s);
        next = digits;
    }
    if ( s->stored )
    {
        stored = "yes";
    }
    (void)snprintf(tail, sizeof(tail), " owner=%s next=%s stored=%s", owner, next, stored);
    (void)snprintf(head, sizeof(head), ">|cron name=%s when=%s cmd=",
                   console_quote(qn, sizeof(qn), s->name),
                   console_quote(qw, sizeof(qw), s->when));
    fixed = strlen(head) + strlen(tail);
    if ( ((KODE_LOG_LINE_MAX - 1U) - CRON_CMD_ROOM_MIN) > fixed )
    {
        room = (KODE_LOG_LINE_MAX - 1U) - fixed;
    }
    if ( '\0' != s->cmd[0] )
    {
        cmd = console_kv_fit(qc, room, s->cmd, "cmd");
    }
    console_reply("%s%s%s", head, cmd, tail);
}

/**
 * @brief Whether the line of an entry opens with a verb that can run from the
 *        scheduler.
 *
 * Asked of the console's own tables, in the console's own task, so nothing is
 * posted. The verb must exist in this build, and must not be @p own: `cron`
 * refuses the scheduler's lines, so an entry that names it could never run.
 *
 * @param line The entry's line.
 * @param own  The verb that refuses the scheduler's lines, `cron` itself.
 * @return true when the line's first word is a verb that may run.
 */
static bool cron_line_runs(const char *line, const char *own)
{
    char  copy[KODE_SCHED_CMD_MAX];
    char *cursor = copy;
    char *verb = NULL;
    bool  runs = false;

    /* The console's own splitter cuts the first word, in place: it works on a copy. */
    (void)snprintf(copy, sizeof(copy), "%s", line);
    if ( CONSOLE_TOK_WORD == console_token(&cursor, &verb) )
    {
        runs = (strcmp(verb, own) != 0)
               && (console_system_verb(verb) || (kverbs_find(verb, NULL) != NULL));
    }
    return runs;
}

/**
 * @brief cron: list the entries, show one, set one, or remove one.
 *
 * ```
 * cron                                          every entry
 * cron name="x"                                 one
 * cron name="x" when="0 7 * * *" cmd="..."      set (`mqtt` takes no cmd)
 * cron name="x" when=off                        remove (`mqtt`: switch off)
 * ```
 *
 * `when` is quoted; `off` is its one bare word, refused when quoted. A line
 * the scheduler runs may not edit the table, and a `cmd` that opens with an
 * unknown verb, or with `cron`, is refused as `msg=cmd`.
 */
static void h_cron(const kverb_t *c, const karg_val_t *v)
{
    bool const has_name = v[CRON_ARG_NAME].present;
    bool const has_when = v[CRON_ARG_WHEN].present;
    bool const has_cmd  = v[CRON_ARG_CMD].present;
    bool posted = false;

    if ( !console_src_allows(CONSOLE_NEED_NOT_SCHED) )
    {
        return;
    }
    if ( has_cmd && !has_when )
    {
        console_reply_err(KCLI_ERR_MISSING, "when");
        return;
    }
    if ( has_when && !has_name )
    {
        console_reply_err(KCLI_ERR_MISSING, "name");
        return;
    }
    if ( has_when )
    {
        bool const off = (strcmp(v[CRON_ARG_WHEN].s, KODE_SCHED_WHEN_OFF) == 0);

        /* Quoted for an expression, bare for `off`, and nothing else: a
         * quoted "off" and a bare expression are both the other form. */
        if ( v[CRON_ARG_WHEN].quoted == off )
        {
            console_reply_err(KCLI_ERR_VALUE, "when");
            return;
        }
        if ( off && has_cmd )
        {
            console_reply_err(KCLI_ERR_VALUE, "cmd");
            return;
        }
        if ( has_cmd && (!cron_line_runs(v[CRON_ARG_CMD].s, c->verb)) )
        {
            /* A word that is no verb, or the verb that refuses these lines:
             * it would be stored and fail every time it fired. */
            console_reply_err(KCLI_ERR_VALUE, "cmd");
            return;
        }
    }
    console_begin(c->verb);
    s_cron_asked = true;
    if ( !has_when )
    {
        const char *name = NULL;

        if ( has_name )
        {
            name = v[CRON_ARG_NAME].s;
        }
        posted = sched_request_list(AO_Console, name);
    }
    else
    {
        const char *line = NULL;

        if ( has_cmd )
        {
            line = v[CRON_ARG_CMD].s;
        }
        posted = sched_request_set(AO_Console, v[CRON_ARG_NAME].s,
                                   v[CRON_ARG_WHEN].s, line);
    }
    if ( !posted )
    {
        console_reply_err(KCLI_ERR_REJECT, c->verb);
        s_cron_asked = false;
        console_end();
    }
}

/**
 * @brief AO_Scheduler's answer to `cron`, posted to AO_Console alone.
 *
 * @param e The event.
 * @return true when it was the answer of `cron`.
 */
static bool scheduler_answer(QEvt const *e)
{
    bool handled = false;

    if ( s_cron_asked && ((QSignal)SCHED_ENTRY_SIG == e->sig) )
    {
        cron_record(Q_EVT_CAST(SchedEntryEvt));
        handled = true;
    }
    else if ( s_cron_asked && ((QSignal)SCHED_END_SIG == e->sig) )
    {
        SchedEndEvt const * const end = Q_EVT_CAST(SchedEndEvt);

        if ( (kode_status_t)end->st == KODE_OK )
        {
            console_reply(">ok");
        }
        else
        {
            cron_refuse(end->why);
        }
        s_cron_asked = false;
        console_end();
        handled = true;
    }
    else
    {
        /* Not the answer of `cron`. */
    }
    return handled;
}

/**
 * @brief The watchdog fired on `cron`: forget it, and let the console answer the timeout.
 *
 * @return false: the console closes the command.
 */
static bool scheduler_timeout(void)
{
    s_cron_asked = false;
    return false;
}

/* `cron [name=] [when=] [cmd=]`. `when` is a KARG_ROW: quoted for an expression, bare for `off`. */
static const karg_spec_t A_CRON[] = {
    { .key = "name", .required = false, .type = KARG_STRING },
    { .key = "when", .required = false, .type = KARG_ROW    },
    { .key = "cmd",  .required = false, .type = KARG_STRING },
};

static const kverb_t SCHEDULER_VERBS[] = {
    { "cron", A_CRON, KVERB_KEYS(A_CRON), h_cron },
};

const kverbs_t scheduler_verbs = {
    .verbs   = SCHEDULER_VERBS,
    .n       = sizeof(SCHEDULER_VERBS) / sizeof(SCHEDULER_VERBS[0]),
    .answer  = scheduler_answer,
    .timeout = scheduler_timeout,
};
